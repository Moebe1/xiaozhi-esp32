// ZZPET-BOT v1.2.8 — ESP32-S3 quadruped pet robot.
// Pinout source: empirical + disassembly verification (see board README).
// Audio path mirrors otto-robot (NoAudioCodecSimplex; one I2S peripheral at a time).

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "led/circular_strip.h"
#include "adc_battery_monitor.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "ZzpetS3"

namespace {

// 50 Hz servo, 13-bit duty -> period = 8192 ticks for 20 ms.
// Hobby SG90 clones in this dog mechanically reach more like 200° if
// you extend the pulse-width window to 0.6 ms .. 2.4 ms (vs. the
// "spec" 1.0..2.0 ms which only gives ~half the visible motion on
// this board's leg geometry).
// 0.6 ms / 20 ms * 8192 ≈ 245, 2.4 ms / 20 ms * 8192 ≈ 983.
constexpr uint32_t kServoMinDuty = 245;
constexpr uint32_t kServoMaxDuty = 983;

uint32_t AngleToDuty(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    return kServoMinDuty + (kServoMaxDuty - kServoMinDuty) * angle / 180;
}

// ----- Leg control helpers (moved out of the class so the motion task
// functions below can call them without an instance). -----
//
// Channel mapping per InitializeServos():
//   LEDC_CHANNEL_1 = FR (GPIO 13), CHANNEL_2 = BR (GPIO 14),
//   LEDC_CHANNEL_3 = FL (GPIO 17), CHANNEL_4 = BL (GPIO 18).
ledc_channel_t LegChannel(int leg) {
    switch (leg) {
        case 1: return LEDC_CHANNEL_3; // FL
        case 2: return LEDC_CHANNEL_1; // FR
        case 3: return LEDC_CHANNEL_4; // BL
        case 4: return LEDC_CHANNEL_2; // BR
        default: return LEDC_CHANNEL_1;
    }
}

// Left-side legs are mirror-mounted. Body-coordinate convention:
//   90 = leg down (neutral), 180 = leg forward (nose), 0 = leg back (tail).
int LogicalToPhysical(int leg, int logical) {
    bool is_left = (leg == 1 || leg == 3); // FL or BL
    return is_left ? (180 - logical) : logical;
}

void SetLegAngle(int leg, int logical_angle) {
    ledc_channel_t ch = LegChannel(leg);
    int physical = LogicalToPhysical(leg, logical_angle);
    ledc_set_duty(SERVO_LEDC_MODE, ch, AngleToDuty(physical));
    ledc_update_duty(SERVO_LEDC_MODE, ch);
}

void AllLegsNeutral() {
    for (int leg = 1; leg <= 4; leg++) SetLegAngle(leg, 90);
}

// ----- Motion task system -----
// Long-running MCP tools (those with vTaskDelay loops) are dispatched on
// the main FreeRTOS task by xiaozhi's MCP server, which blocks the event
// loop (audio, state machine, MCP scheduling) for the lambda's full
// duration. To keep the main loop responsive we offload them to a
// dedicated short-lived task, serialized by `g_motion_mutex` so two
// motions can't fight over the servos.

SemaphoreHandle_t g_motion_mutex = nullptr;

using MotionFn = void(*)(int);

struct MotionJob {
    MotionFn fn;
    int arg;
};

void MotionTaskBody(void* p) {
    MotionJob* job = static_cast<MotionJob*>(p);
    if (job != nullptr) {
        if (job->fn != nullptr) job->fn(job->arg);
        // NOTE: do NOT auto-reset to neutral here. Each motion function
        // owns its end state — pose-style tools (sit, bow, shake_paw)
        // need to HOLD their final position until the next command.
        // Animation-style tools (wave, walk, scratch, …) return to
        // neutral themselves at the end of their sequence.
        delete job;
    }
    xSemaphoreGive(g_motion_mutex);
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGD(TAG, "motion task done, stack high-water mark = %u bytes",
             (unsigned)(hwm * sizeof(StackType_t)));
    vTaskDelete(nullptr);
}

// Returns true if the motion was launched, false if a motion was already
// running (request dropped). Tool callbacks return this so the LLM can
// retry / acknowledge appropriately.
//
// 8 KiB stack: the motion functions themselves are tiny but each
// SetLegAngle hits ledc_set_duty + ledc_update_duty which in IDF pulls in
// HAL + lock helpers that easily eat 1-2 KiB of frame. 3 KiB was too tight
// and overflowed during the post-motion AllLegsNeutral on some calls.
constexpr uint32_t kMotionStack = 8192;

bool TryStartMotion(MotionFn fn, int arg = 0) {
    if (g_motion_mutex == nullptr) return false;
    if (xSemaphoreTake(g_motion_mutex, 0) != pdTRUE) return false;
    MotionJob* job = new MotionJob{fn, arg};
    BaseType_t ok = xTaskCreate(MotionTaskBody, "robo_motion",
                                 kMotionStack, job, 4, nullptr);
    if (ok != pdPASS) {
        delete job;
        xSemaphoreGive(g_motion_mutex);
        return false;
    }
    return true;
}

void EnsureMotionMutex() {
    if (g_motion_mutex == nullptr) {
        // Binary semaphore (NOT a mutex): mutexes have priority inheritance
        // and MUST be released by the taking task, but our pattern takes in
        // the calling task (main loop) and releases in the motion task —
        // which trips FreeRTOS's `xTaskPriorityDisinherit` assert and panics
        // the system after every successful motion. Binary semaphore has no
        // ownership tracking, so cross-task give/take is fine.
        g_motion_mutex = xSemaphoreCreateBinary();
        xSemaphoreGive(g_motion_mutex);  // start in the "available" state
    }
}

// ----- Motion functions (run on the dedicated task) -----

// --- Animation-style motions: end at NEUTRAL ---

void Task_Wave(int leg) {
    SetLegAngle(leg, 180);
    vTaskDelay(pdMS_TO_TICKS(500));
    for (int i = 0; i < 3; i++) {
        SetLegAngle(leg,  60); vTaskDelay(pdMS_TO_TICKS(260));
        SetLegAngle(leg, 180); vTaskDelay(pdMS_TO_TICKS(260));
    }
    SetLegAngle(leg, 90);
}

void Task_WaveHello(int) {
    Task_Wave(2);  // front-right
}

void Task_Scratch(int leg) {
    if (leg < 3 || leg > 4) leg = 4;
    for (int i = 0; i < 6; i++) {
        SetLegAngle(leg, 130); vTaskDelay(pdMS_TO_TICKS(120));
        SetLegAngle(leg,  60); vTaskDelay(pdMS_TO_TICKS(120));
    }
    SetLegAngle(leg, 90);
}

void Task_HappyWiggle(int) {
    for (int i = 0; i < 5; i++) {
        SetLegAngle(1, 110); SetLegAngle(2, 110);
        SetLegAngle(3,  70); SetLegAngle(4,  70);
        vTaskDelay(pdMS_TO_TICKS(140));
        SetLegAngle(1,  70); SetLegAngle(2,  70);
        SetLegAngle(3, 110); SetLegAngle(4, 110);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
    AllLegsNeutral();
}

void Task_Walk(int steps) {
    if (steps < 1) steps = 1;
    if (steps > 5) steps = 5;  // hard cap: 5 steps × 500 ms = 2.5 s max
    for (int s = 0; s < steps; s++) {
        SetLegAngle(1,  60); SetLegAngle(4,  60);
        SetLegAngle(2, 120); SetLegAngle(3, 120);
        vTaskDelay(pdMS_TO_TICKS(250));
        SetLegAngle(2,  60); SetLegAngle(3,  60);
        SetLegAngle(1, 120); SetLegAngle(4, 120);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    AllLegsNeutral();
}

void Task_Jump(int) {
    // Gather all four legs inward toward the body...
    SetLegAngle(1,  30); SetLegAngle(2,  30);
    SetLegAngle(3, 150); SetLegAngle(4, 150);
    vTaskDelay(pdMS_TO_TICKS(280));
    // ...then SNAP back to stand position (per spec).
    AllLegsNeutral();
}

// --- Pose-style motions: HOLD their final position ---

void Task_ShakePaw(int) {
    SetLegAngle(2, 170);
    // Hold indefinitely; next command takes the dog out of this pose.
}

void Task_Bow(int) {
    SetLegAngle(1, 170); SetLegAngle(2, 170);
    // Hold the bow until the next command.
}

void Task_SetPose(int pose) {
    int fl = 90, fr = 90, bl = 90, br = 90;
    switch (pose) {
        case 2: bl = 30; br = 30; break;
        case 3: fl = 180; fr = 180; bl = 0; br = 0; break;
        case 4: fl = 150; fr = 150; break;
        default: break;
    }
    SetLegAngle(1, fl); SetLegAngle(2, fr);
    SetLegAngle(3, bl); SetLegAngle(4, br);
    // Hold this pose until the next command (no auto-revert).
}

// ----- LED palette for the bottom-strip MCP tool -----
struct RGB { uint8_t r, g, b; };
constexpr RGB kPalette[] = {
    {  0,   0,   0},  // 0 off
    {120,   0,   0},  // 1 red
    {  0, 120,   0},  // 2 green
    {  0,   0, 120},  // 3 blue
    {120, 100,   0},  // 4 yellow
    {120,   0, 120},  // 5 magenta
    {  0, 120, 120},  // 6 cyan
    { 80,  80,  80},  // 7 white
};
constexpr int kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

CircularStrip& BottomStrip() {
    static CircularStrip strip(BOTTOM_LED_GPIO, BOTTOM_LED_COUNT);
    return strip;
}

} // namespace

class ZzpetS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    AdcBatteryMonitor* battery_monitor_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeOledDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_LOGI(TAG, "Install SH1106 driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize OLED");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // The tny-robotics SH1106 driver maps mirror_x → 0xA6/0xA7 (which
        // are display NORMAL/REVERSE = color invert, *not* segment remap),
        // so there's no way to ask the driver to flip horizontally via the
        // public API. The init sequence sends 0xA1 (SEG_REMAP_INVERSE), so
        // the panel is X-mirrored at idle. Override by sending 0xA0
        // (SEG_REMAP_NORMAL) here, AFTER LVGL has finished its own mirror
        // call inside OledDisplay's constructor.
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0xA0, NULL, 0));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([]() {
            Application::GetInstance().StopListening();
        });
    }

    void InitializeServos() {
        ledc_timer_config_t timer_cfg = {
            .speed_mode = SERVO_LEDC_MODE,
            .duty_resolution = SERVO_LEDC_RESOLUTION,
            .timer_num = SERVO_LEDC_TIMER,
            .freq_hz = SERVO_LEDC_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        const gpio_num_t pins[4] = {SERVO_FR_GPIO, SERVO_BR_GPIO, SERVO_FL_GPIO, SERVO_BL_GPIO};
        // Reserve channels 1..4 (channel 0 conventionally used by backlight helpers).
        const ledc_channel_t channels[4] = {LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4};
        for (int i = 0; i < 4; ++i) {
            ledc_channel_config_t ch_cfg = {
                .gpio_num = pins[i],
                .speed_mode = SERVO_LEDC_MODE,
                .channel = channels[i],
                .intr_type = LEDC_INTR_DISABLE,
                .timer_sel = SERVO_LEDC_TIMER,
                .duty = AngleToDuty(90),
                .hpoint = 0,
                .flags = {.output_invert = 0},
            };
            ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
        }
    }

    void InitializeBatteryMonitor() {
        battery_monitor_ = new AdcBatteryMonitor(
            BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
            BATTERY_VOLTAGE_UPPER_RESISTOR, BATTERY_VOLTAGE_LOWER_RESISTOR,
            GPIO_NUM_NC);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // === Synchronous tools (instant; safe to run on main loop) ===

        // Atomic leg control. Angle is body-coords (90 = down, 180 = fwd, 0 = back).
        // Left-side servos are mirrored internally — uniform for all 4 legs.
        mcp_server.AddTool(
            "self.pet.move_leg",
            "Set one leg to a body-coordinate angle. leg: 1=front-left, "
            "2=front-right, 3=back-left, 4=back-right. angle: 0-180 in body "
            "frame — 90 = straight down (neutral), 180 = fully forward, "
            "0 = fully back. Same convention for all 4 legs.",
            PropertyList({
                Property("leg",   kPropertyTypeInteger, 1, 1, 4),
                Property("angle", kPropertyTypeInteger, 90, 0, 180),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                SetLegAngle(properties["leg"].value<int>(),
                            properties["angle"].value<int>());
                return true;
            });

        // Bottom-strip lights (2-LED chain on GPIO 8). The WiFi LED on
        // GPIO 48 is state-driven by the application — only this strip is
        // user-controllable.
        mcp_server.AddTool(
            "self.pet.set_lights",
            "Set the colour of the dog's two bottom NeoPixel lights. "
            "color: 0=off, 1=red, 2=green, 3=blue, 4=yellow, 5=magenta, "
            "6=cyan, 7=white.",
            PropertyList({
                Property("color", kPropertyTypeInteger, 0, 0, 7),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int idx = properties["color"].value<int>();
                if (idx < 0 || idx >= kPaletteCount) idx = 0;
                const RGB c = kPalette[idx];
                BottomStrip().SetAllColor({c.r, c.g, c.b});
                return true;
            });

        // === Background-task tools (long delays; offloaded to robo_motion task) ===
        // Each returns false to the LLM if a motion is already running, so the
        // model can decide whether to acknowledge or retry.

        mcp_server.AddTool(
            "self.pet.set_pose",
            "Set whole-body pose. pose: 1=stand, 2=sit (back legs back 60°), "
            "3=lie_down (front fold forward 90°, back fold back 90°), "
            "4=alert (front legs raise forward 60°). Runs in background — "
            "returns true if started, false if another motion is already in "
            "progress.",
            PropertyList({
                Property("pose", kPropertyTypeInteger, 1, 1, 4),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return TryStartMotion(Task_SetPose, properties["pose"].value<int>());
            });

        mcp_server.AddTool(
            "self.pet.wave",
            "Wave a leg as a greeting. Raises the leg fully, then swings "
            "60↔180 three times. leg: 1=FL, 2=FR, 3=BL, 4=BR (default 2).",
            PropertyList({
                Property("leg", kPropertyTypeInteger, 2, 1, 4),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return TryStartMotion(Task_Wave, properties["leg"].value<int>());
            });

        mcp_server.AddTool(
            "self.pet.shake_paw",
            "Offer the front-right paw for a handshake — raise FR high and "
            "hold ~2.5 s.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return TryStartMotion(Task_ShakePaw, 0);
            });

        mcp_server.AddTool(
            "self.pet.bow",
            "Bow: stretch both front legs forward, hold ~1.8 s.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return TryStartMotion(Task_Bow, 0);
            });

        mcp_server.AddTool(
            "self.pet.scratch",
            "Scratch with one back leg — quick small oscillations. "
            "leg: 3=back-left, 4=back-right (default 4).",
            PropertyList({
                Property("leg", kPropertyTypeInteger, 4, 3, 4),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return TryStartMotion(Task_Scratch, properties["leg"].value<int>());
            });

        mcp_server.AddTool(
            "self.pet.happy_wiggle",
            "Wiggle excitedly — front-pair and back-pair alternating sway.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return TryStartMotion(Task_HappyWiggle, 0);
            });

        mcp_server.AddTool(
            "self.pet.walk",
            "Walk forward by alternating diagonal leg pairs in a trot. "
            "steps: 1-5 (capped, default 3).",
            PropertyList({
                Property("steps", kPropertyTypeInteger, 3, 1, 5),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                return TryStartMotion(Task_Walk, properties["steps"].value<int>());
            });

        mcp_server.AddTool(
            "self.pet.jump",
            "Jump: fold all four legs inward, then snap back to stand "
            "(boing gesture — 1-DOF legs can't actually leave the ground).",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return TryStartMotion(Task_Jump, 0);
            });

        // Back-compat shim — same as wave with leg=2.
        mcp_server.AddTool(
            "self.pet.wave_hello",
            "Wave hello with the front-right leg.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return TryStartMotion(Task_WaveHello, 0);
            });
    }

public:
    ZzpetS3Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO) {
        EnsureMotionMutex();
        InitializeDisplayI2c();
        InitializeOledDisplay();
        InitializeButtons();
        InitializeServos();
        InitializeBatteryMonitor();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        // WiFi status LED (single WS2812 on GPIO 48).
        static SingleLed wifi_led(BUILTIN_LED_GPIO);
        return &wifi_led;
    }

    // Two-LED bottom strip — exposed so future code (or MCP tools) can drive it.
    CircularStrip* GetBottomStrip() {
        static CircularStrip strip(BOTTOM_LED_GPIO, BOTTOM_LED_COUNT);
        return &strip;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // +10 % mic gain (needs the float-aware multiplier patch in
        // main/audio/codecs/no_audio_codec.cc — pre-patch this is a no-op
        // because the int cast truncates 1.1 → 1 → unity gain).
        static bool gain_initialized = false;
        if (!gain_initialized) {
            audio_codec.SetInputGain(1.1f);
            gain_initialized = true;
        }
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_monitor_ == nullptr) {
            return false;
        }
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(ZzpetS3Board);
