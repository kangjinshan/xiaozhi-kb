#include "keyboard_app.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "button.h"
#include "config.h"
#include "app_mode.h"
#include "ble_hid_keyboard.h"

#define TAG "keyboard_app"

void RunKeyboardApp() {
    auto& kb = BleHidKeyboard::GetInstance();
    kb.Init();

    // 左键：按住=右Option
    static Button left(BOOT_BUTTON_GPIO);
    left.OnPressDown([&kb]() { kb.SendModifier(HID_MOD_RIGHT_ALT, true); });
    left.OnPressUp([&kb]()  { kb.SendModifier(HID_MOD_RIGHT_ALT, false); });

    // 中键：短按=退格；长按2s=回选择界面
    static Button mid(KEY_MID_GPIO, false, 2000, 0);
    mid.OnClick([&kb]()     { kb.TapKey(HID_KEY_BACKSPACE); });
    mid.OnLongPress([]()    { AppModeWriteAndReboot(AppMode::kSelector); });

    // 右键：单击=回车
    static Button right(KEY_RIGHT_GPIO);
    right.OnClick([&kb]()   { kb.TapKey(HID_KEY_ENTER); });

    kb.SetConnectionCallback([](bool c) {
        ESP_LOGI(TAG, "BLE %s", c ? "connected" : "disconnected");
        // 屏幕状态更新在 Task 7 接入 Display
    });

    ESP_LOGI(TAG, "keyboard app running");
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
