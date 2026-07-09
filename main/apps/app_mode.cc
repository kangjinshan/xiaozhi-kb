#include "app_mode.h"
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "app_mode"
static const char* kNs = "appsel";
static const char* kKey = "mode";
static const char* kKeyboardProfileKey = "keyboard_profile";

static const char* AppModeToStr(AppMode m) {
    switch (m) {
        case AppMode::kXiaozhi:  return "xiaozhi";
        case AppMode::kKeyboard: return "keyboard";
        default:                 return "selector";
    }
}

AppMode AppModeRead() {
    nvs_handle_t h;
    esp_err_t oerr = nvs_open(kNs, NVS_READONLY, &h);
    ESP_LOGW(TAG, "DIAG nvs_open(%s)=%d (%s)", kNs, oerr, esp_err_to_name(oerr));
    if (oerr != ESP_OK) return AppMode::kSelector;
    char buf[16] = {0};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, kKey, buf, &len);
    ESP_LOGW(TAG, "DIAG nvs_get_str(%s)=%d (%s) len=%d val='%s'", kKey, err, esp_err_to_name(err), (int)len, buf);
    nvs_close(h);
    if (err != ESP_OK) return AppMode::kSelector;
    if (strcmp(buf, "xiaozhi") == 0)  return AppMode::kXiaozhi;
    if (strcmp(buf, "keyboard") == 0) return AppMode::kKeyboard;
    return AppMode::kSelector;
}

KeyboardProfile KeyboardProfileRead() {
    nvs_handle_t h;
    esp_err_t oerr = nvs_open(kNs, NVS_READONLY, &h);
    if (oerr != ESP_OK) {
        return KeyboardProfile::kProfile1;
    }

    int32_t value = 1;
    esp_err_t err = nvs_get_i32(h, kKeyboardProfileKey, &value);
    nvs_close(h);

    if (err != ESP_OK) {
        return KeyboardProfile::kProfile1;
    }
    if (value == static_cast<int32_t>(KeyboardProfile::kProfile2)) {
        return KeyboardProfile::kProfile2;
    }
    return KeyboardProfile::kProfile1;
}

static void KeyboardProfileWrite(nvs_handle_t h, KeyboardProfile profile) {
    ESP_ERROR_CHECK(nvs_set_i32(
        h, kKeyboardProfileKey, static_cast<int32_t>(profile)));
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

void AppModeWriteKeyboardAndReboot(KeyboardProfile profile) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, kKey, AppModeToStr(AppMode::kKeyboard));
        KeyboardProfileWrite(h, profile);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open failed, reboot anyway");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
