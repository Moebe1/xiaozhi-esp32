#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();

    esp_timer_create_args_t reconnect_args = {
        .callback = [](void* arg) {
            static_cast<WebsocketProtocol*>(arg)->AttemptReconnect();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_reconnect",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&reconnect_args, &reconnect_timer_);
}

WebsocketProtocol::~WebsocketProtocol() {
    ESP_LOGI(TAG, "WebsocketProtocol deinit");
    // Mark dead first so any pending scheduled callback no-ops.
    *alive_ = false;
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    websocket_.reset();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    // Explicit close — cancel any pending auto-reconnect so we don't fight it.
    should_reconnect_ = false;
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    reconnect_attempts_ = 0;
    ESP_LOGI(TAG, "CloseAudioChannel (explicit)");
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    // Cancel any pending auto-reconnect — this open is the authoritative attempt.
    // We will re-arm should_reconnect_ on the success path at the bottom of this function.
    should_reconnect_ = false;
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }

    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected (should_reconnect=%d attempts=%d free_heap=%u)",
                 should_reconnect_.load(),
                 reconnect_attempts_.load(),
                 (unsigned)esp_get_free_heap_size());
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
        if (should_reconnect_.load()) {
            ScheduleReconnect();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    // Channel is fully up — arm auto-reconnect for unexpected drops.
    should_reconnect_ = true;
    reconnect_attempts_ = 0;
    ESP_LOGI(TAG, "Audio channel opened, auto-reconnect armed");

    return true;
}

int WebsocketProtocol::CurrentBackoffMs() const {
    // Exponential 1s, 2s, 4s, 8s, 16s, then capped at WEBSOCKET_RECONNECT_MAX_DELAY_MS.
    int attempt = reconnect_attempts_.load();
    if (attempt < 0) attempt = 0;
    if (attempt > 20) attempt = 20;  // saturate the shift
    int delay = WEBSOCKET_RECONNECT_INITIAL_DELAY_MS << attempt;
    if (delay > WEBSOCKET_RECONNECT_MAX_DELAY_MS || delay <= 0) {
        delay = WEBSOCKET_RECONNECT_MAX_DELAY_MS;
    }
    return delay;
}

void WebsocketProtocol::ScheduleReconnect() {
    if (!should_reconnect_.load()) {
        return;
    }
    int attempt = reconnect_attempts_.load();
    if (attempt >= WEBSOCKET_RECONNECT_MAX_RETRIES) {
        ESP_LOGE(TAG, "WS reconnect retries exhausted (%d/%d), giving up; user must wake to reconnect",
                 attempt, WEBSOCKET_RECONNECT_MAX_RETRIES);
        should_reconnect_ = false;
        return;
    }
    int delay_ms = CurrentBackoffMs();
    ESP_LOGI(TAG, "WS reconnect: scheduling attempt %d/%d in %d ms",
             attempt + 1, WEBSOCKET_RECONNECT_MAX_RETRIES, delay_ms);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);  // safety against double-arm
        esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000);
    }
}

void WebsocketProtocol::AttemptReconnect() {
    // Runs in the esp_timer task. We must NOT do blocking work here.
    // Hand off to the app event loop via Schedule, guarded by alive_.
    if (!(*alive_) || !should_reconnect_.load()) {
        return;
    }
    auto alive = alive_;
    Application::GetInstance().Schedule([this, alive]() {
        if (!*alive || !should_reconnect_.load()) {
            return;
        }
        // Only reconnect from idle. If the app is mid-connect already, leave it alone.
        auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateIdle) {
            ESP_LOGI(TAG, "WS reconnect: device state=%d not idle, deferring", (int)state);
            ScheduleReconnect();
            return;
        }
        int attempt = ++reconnect_attempts_;  // bump before attempt
        ESP_LOGI(TAG, "WS reconnect: attempt %d (free_heap=%u)",
                 attempt, (unsigned)esp_get_free_heap_size());
        // OpenAudioChannel clears should_reconnect_ on entry and re-arms on success.
        // If it fails we re-arm explicitly and schedule the next try.
        if (OpenAudioChannel()) {
            ESP_LOGI(TAG, "WS reconnect: succeeded on attempt %d", attempt);
            // OpenAudioChannel already set should_reconnect_=true and zeroed attempts.
        } else {
            ESP_LOGW(TAG, "WS reconnect: attempt %d failed", attempt);
            should_reconnect_ = true;  // OpenAudioChannel cleared it
            ScheduleReconnect();
        }
    });
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
