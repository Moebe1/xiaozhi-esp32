// Diagnostic firmware for unknown ESP32-S3 hardware.
// Inherits from WifiBoard (which supplies default network impls) but runs a
// background task that does a GPIO floating-state survey and an I2C scan,
// logging results over USB-Serial-JTAG. Avoids GPIO 19/20 (USB D-/D+) so the
// host stays connected throughout.
//
// Build:
//   python scripts/release.py probe-gpios
// Flash + monitor:
//   idf.py -p /dev/cu.usbmodem101 -b 460800 flash monitor

#include "wifi_board.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Probe"

namespace {

// Safe-to-touch GPIOs on ESP32-S3-N16R8.
// Excluded: 19/20 (USB D-/D+), 22-25 (don't exist on S3), 26-32 (flash/PSRAM).
// 0/3/45/46 are bootstrap pins; we only read them as inputs.
constexpr int kCandidates[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18,
    21,
    33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44,
    45, 46, 47, 48,
};
constexpr int kNumCandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);

bool IsUsbPin(int gpio) { return gpio == 19 || gpio == 20; }

void SurveyGpios() {
    ESP_LOGI(TAG, "--- floating-state survey ---");
    ESP_LOGI(TAG, "  legend: pu/pd are levels read with internal pull-up/down enabled");
    ESP_LOGI(TAG, "  pu=1 pd=1 -> external pull-up present (I2C / button candidate)");
    ESP_LOGI(TAG, "  pu=0 pd=0 -> tied LOW externally");
    ESP_LOGI(TAG, "  pu=1 pd=0 -> floating");

    for (int i = 0; i < kNumCandidates; i++) {
        int gpio = kCandidates[i];
        gpio_reset_pin(static_cast<gpio_num_t>(gpio));
        gpio_set_direction(static_cast<gpio_num_t>(gpio), GPIO_MODE_INPUT);

        gpio_set_pull_mode(static_cast<gpio_num_t>(gpio), GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_pu = gpio_get_level(static_cast<gpio_num_t>(gpio));

        gpio_set_pull_mode(static_cast<gpio_num_t>(gpio), GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_pd = gpio_get_level(static_cast<gpio_num_t>(gpio));

        gpio_set_pull_mode(static_cast<gpio_num_t>(gpio), GPIO_FLOATING);

        const char* tag = "";
        if (with_pu == 1 && with_pd == 1)       tag = " <-- EXTERNAL PULL-UP (I2C/button candidate)";
        else if (with_pu == 0 && with_pd == 0)  tag = " <-- tied LOW externally";

        ESP_LOGI(TAG, "  GPIO %2d  pu=%d pd=%d%s", gpio, with_pu, with_pd, tag);
    }
}

void I2cScan() {
    ESP_LOGI(TAG, "--- I2C scan for SSD1306 (0x3C / 0x3D) on all pin pairs ---");
    int total_pairs = (kNumCandidates - 2) * (kNumCandidates - 3); // approx, after excluding USB
    ESP_LOGI(TAG, "(testing up to ~%d pairs; takes ~20 s)", total_pairs);

    int found = 0;
    for (int i = 0; i < kNumCandidates; i++) {
        int sda = kCandidates[i];
        if (IsUsbPin(sda)) continue;

        for (int j = 0; j < kNumCandidates; j++) {
            int scl = kCandidates[j];
            if (sda == scl || IsUsbPin(scl)) continue;

            i2c_master_bus_config_t bus_cfg = {};
            bus_cfg.i2c_port = I2C_NUM_0;
            bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda);
            bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl);
            bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
            bus_cfg.glitch_ignore_cnt = 7;
            bus_cfg.intr_priority = 0;
            bus_cfg.trans_queue_depth = 0;
            bus_cfg.flags.enable_internal_pullup = 1;

            i2c_master_bus_handle_t bus_h = nullptr;
            esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_h);
            if (err != ESP_OK) continue;

            const uint8_t addrs[] = {0x3C, 0x3D};
            for (uint8_t addr : addrs) {
                if (i2c_master_probe(bus_h, addr, 30) == ESP_OK) {
                    ESP_LOGW(TAG, "*** I2C device 0x%02X responds at SDA=%d SCL=%d ***",
                             addr, sda, scl);
                    found++;
                }
            }
            i2c_del_master_bus(bus_h);
        }
        if ((i & 3) == 0) {
            ESP_LOGI(TAG, "  progress %d/%d (found=%d)", i, kNumCandidates, found);
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %d hit(s)", found);
    if (found == 0) {
        ESP_LOGW(TAG, "no I2C devices found at 0x3C/0x3D");
        ESP_LOGW(TAG, "display might be SPI, or on a different I2C address");
    }
}

void ProbeTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // let boot logs settle first
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  ESP32-S3 GPIO probe firmware                    ");
    ESP_LOGI(TAG, "==================================================");
    SurveyGpios();
    I2cScan();
    ESP_LOGI(TAG, "==== probe complete; idle loop ====");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "(idle — paste log above to identify OLED pins)");
    }
}

}  // namespace

class ProbeBoard : public WifiBoard {
public:
    ProbeBoard() : WifiBoard() {
        xTaskCreate(ProbeTask, "probe", 8192, nullptr, 5, nullptr);
    }
};

DECLARE_BOARD(ProbeBoard);
