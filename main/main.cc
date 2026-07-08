#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "app_mode.h"
#include "keyboard_app.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Dispatch based on the app mode stored in NVS
    AppMode mode = AppModeRead();
    if (mode == AppMode::kKeyboard) {
        ESP_LOGI(TAG, "boot -> keyboard app");
        RunKeyboardApp();  // Runs the BLE keyboard app and never returns
        return;
    }

    // kXiaozhi and kSelector (before Task 8) both fall through to xiaozhi
    ESP_LOGI(TAG, "boot -> xiaozhi");
    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
