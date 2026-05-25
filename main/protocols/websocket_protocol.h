#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <atomic>
#include <memory>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

// Auto-reconnect on unexpected drop. Exponential backoff capped at MAX_DELAY,
// give up after MAX_RETRIES so a dead server doesn't pin a busy reconnect loop.
// 1 + 2 + 4 + 8 + 16 + 30 + 30 + 30 ≈ 121 s total before giving up.
#define WEBSOCKET_RECONNECT_INITIAL_DELAY_MS 1000
#define WEBSOCKET_RECONNECT_MAX_DELAY_MS     30000
#define WEBSOCKET_RECONNECT_MAX_RETRIES      8

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // Set to false in dtor so any in-flight scheduled callback no-ops instead of
    // dereferencing freed memory. Matches mqtt_protocol.cc's pattern.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;

    // Auto-reconnect state.
    esp_timer_handle_t reconnect_timer_ = nullptr;
    std::atomic<bool> should_reconnect_{false};
    std::atomic<int> reconnect_attempts_{0};

    void ScheduleReconnect();
    void AttemptReconnect();
    int CurrentBackoffMs() const;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
