#include "app_mode.h"
#include "app_mode_keys.h"
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

static const char* AppModeToStr(AppMode m) {
    switch (m) {
        case AppMode::kXiaozhi:  return "xiaozhi";
        case AppMode::kKeyboard: return "keyboard";
        case AppMode::kRecorder: return "recorder";
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
    if (strcmp(buf, "recorder") == 0) return AppMode::kRecorder;
    return AppMode::kSelector;
}

KeyboardProfile KeyboardProfileRead() {
    nvs_handle_t h;
    esp_err_t oerr = nvs_open(kNs, NVS_READONLY, &h);
    if (oerr != ESP_OK) {
        ESP_LOGW(TAG,
                 "keyboard profile unavailable; using unified keyboard+mouse profile");
        return KeyboardProfile::kProfile2;
    }

    int32_t value = static_cast<int32_t>(KeyboardProfile::kProfile2);
    esp_err_t err = nvs_get_i32(h, KeyboardProfileNvsKey(), &value);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "keyboard profile read failed: %s; using unified keyboard+mouse profile",
                 esp_err_to_name(err));
        return KeyboardProfile::kProfile2;
    }
    if (value != static_cast<int32_t>(KeyboardProfile::kProfile2)) {
        ESP_LOGW(TAG,
                 "legacy keyboard profile=%ld migrated at runtime to keyboard+mouse",
                 static_cast<long>(value));
    } else {
        ESP_LOGI(TAG, "keyboard profile=2 (keyboard+mouse)");
    }
    return KeyboardProfile::kProfile2;
}

static void KeyboardProfileWrite(nvs_handle_t h, KeyboardProfile profile) {
    esp_err_t err = nvs_set_i32(
        h, KeyboardProfileNvsKey(), static_cast<int32_t>(profile));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "keyboard profile write failed: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);
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
