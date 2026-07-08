#include "app_mode.h"
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "app_mode"
static const char* kNs = "appsel";
static const char* kKey = "mode";

static const char* AppModeToStr(AppMode m) {
    switch (m) {
        case AppMode::kXiaozhi:  return "xiaozhi";
        case AppMode::kKeyboard: return "keyboard";
        default:                 return "selector";
    }
}

AppMode AppModeRead() {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return AppMode::kSelector;
    char buf[16] = {0};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, kKey, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) return AppMode::kSelector;
    if (strcmp(buf, "xiaozhi") == 0)  return AppMode::kXiaozhi;
    if (strcmp(buf, "keyboard") == 0) return AppMode::kKeyboard;
    return AppMode::kSelector;
}

void AppModeWriteAndReboot(AppMode mode) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, kKey, AppModeToStr(mode));
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open failed, reboot anyway");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
