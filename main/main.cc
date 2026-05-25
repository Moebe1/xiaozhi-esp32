#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"

static const char* ResetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT (reset pin)";
        case ESP_RST_SW:         return "SW (esp_restart)";
        case ESP_RST_PANIC:      return "PANIC (exception/abort)";
        case ESP_RST_INT_WDT:    return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:   return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:        return "WDT (other watchdog)";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                 return "?";
    }
}

extern "C" void app_main(void)
{
    // Log the reset reason early — critical signal for diagnosing mid-conversation
    // reboots vs clean shutdowns vs panics vs brownouts.
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Boot. reset_reason=%d (%s) free_heap=%u min_free_heap=%u",
             (int)reason, ResetReasonString(reason),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
