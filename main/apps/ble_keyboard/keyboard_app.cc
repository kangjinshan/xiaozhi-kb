#include "keyboard_app.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "button.h"
#include "config.h"
#include "ble_hid_keyboard.h"
#include "keyboard_touch_arrows.h"
#include "keyboard_zone_display.h"
#include "app_mode.h"
#include "sdcard.h"

#define TAG "keyboard_app"

// 蓝牙键鼠应用：亮屏时触摸屏作为触控板，暗屏时恢复九宫格键盘触区；
// GPIO9/GPIO10 为鼠标左/右键，中间 PWR 切换两种输入模式。
void RunKeyboardApp() {
    auto& kb = BleHidKeyboard::GetInstance();
    const KeyboardProfile profile = KeyboardProfileRead();
    kb.Init();

    // AMOLED 与 SD 共用 SPI2。先按屏幕最大传输尺寸建立总线，再让 SD
    // 复用；否则 SD 自建总线的 4 KiB 上限不足以承载 LVGL 刷新。
    esp_err_t display_bus_err = KeyboardZoneDisplayPrepareSharedSpiBus();
    if (display_bus_err == ESP_OK) {
        SdCardMount(false);
    } else {
        ESP_LOGW(TAG,
                 "display SPI bus prepare failed: %s",
                 esp_err_to_name(display_bus_err));
        SdCardMount(true);
    }

    StartKeyboardTouchArrows(kb, profile);
    // Do not install the synchronous SD log hook in HID mode. BLE host/event
    // callbacks also emit logs; making those callbacks wait for FATFS/SPI can
    // starve the controller until the supervision timeout expires.

    // 最左键 GPIO10：配置2为鼠标右键，配置1仍为右 Option。
    static Button left(KEY_LEFT_GPIO);
    left.OnPressDown([&kb, profile]() {
        if (profile == KeyboardProfile::kProfile2) {
            kb.SendMouseButton(HID_MOUSE_BUTTON_RIGHT, true);
        } else {
            kb.SendModifier(HID_MOD_RIGHT_ALT, true);
        }
    });
    left.OnPressUp([&kb, profile]() {
        if (profile == KeyboardProfile::kProfile2) {
            kb.SendMouseButton(HID_MOUSE_BUTTON_RIGHT, false);
        } else {
            kb.SendModifier(HID_MOD_RIGHT_ALT, false);
        }
    });

    // 最右键（BOOT）：配置2为鼠标左键；配置1仍发送回车。
    static Button right(BOOT_BUTTON_GPIO);
    if (profile == KeyboardProfile::kProfile2) {
        right.OnPressDown([&kb]() {
            kb.SendMouseButton(HID_MOUSE_BUTTON_LEFT, true);
        });
        right.OnPressUp([&kb]() {
            kb.SendMouseButton(HID_MOUSE_BUTTON_LEFT, false);
        });
    } else {
        right.OnClick([&kb]() {
            kb.TapKey(HID_KEY_ENTER);
        });
    }

    kb.SetConnectionCallback([](bool c) {
        ESP_LOGI(TAG, "BLE %s", c ? "connected" : "disconnected");
    });

    if (profile == KeyboardProfile::kProfile2) {
        ESP_LOGI(TAG,
                 "keyboard+touchpad running (bright=touch mouse, "
                 "dark=touch keyboard, "
                 "boot=Mouse Left, gpio10=Mouse Right, "
                 "pwr=Screen Toggle)");
    } else {
        ESP_LOGI(TAG,
                 "keyboard app running (profile=%d, left=Right Option, "
                 "boot=Enter, pwr=Backspace)",
                 static_cast<int>(profile));
    }
    uint32_t heartbeat = 0;
    while (true) {
        ESP_LOGI(TAG,
                 "keyboard heartbeat #%lu connected=%d advertising=%d "
                 "display_init=%d "
                 "display_on=%d display_err=%s",
                 heartbeat++,
                 kb.IsConnected(),
                 kb.IsAdvertising(),
                 KeyboardZoneDisplayInitialized(),
                 KeyboardInputModeDisplayIsOn(),
                 esp_err_to_name(KeyboardZoneDisplayLastError()));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
