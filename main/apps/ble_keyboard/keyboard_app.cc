#include "keyboard_app.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "button.h"
#include "config.h"
#include "ble_hid_keyboard.h"
#include "keyboard_touch_arrows.h"
#include "app_mode.h"

#define TAG "keyboard_app"

// 蓝牙键盘应用：两个物理键
//   最左键 GPIO10：按住 = 右 Option（松开释放）
//   最右键 GPIO9 (BOOT)：单击 = 回车
//   中间 PWR：单击 = 退格（通过 AXP2101 短按 IRQ 识别）
void RunKeyboardApp() {
    // ==== 分段诊断：先确认无BLE时日志正常，再测Init ====
    for (int i = 0; i < 5; i++) {
        ESP_LOGW(TAG, "DIAG before Init, tick=%d", i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "DIAG === calling BleHidKeyboard::Init() now ===");

    auto& kb = BleHidKeyboard::GetInstance();
    const KeyboardProfile profile = KeyboardProfileRead();
    kb.Init();
    StartKeyboardTouchArrows(kb, profile);

    ESP_LOGW(TAG, "DIAG === Init() returned OK ===");
    for (int i = 0; i < 5; i++) {
        ESP_LOGW(TAG, "DIAG after Init, tick=%d", i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 最左键：按住 = 右 Option
    static Button left(KEY_LEFT_GPIO);
    left.OnPressDown([&kb]() { kb.SendModifier(HID_MOD_RIGHT_ALT, true); });
    left.OnPressUp([&kb]()  { kb.SendModifier(HID_MOD_RIGHT_ALT, false); });

    // 最右键（BOOT）：配置1单击=回车；配置2单击=触区图/黑屏切换。
    static Button right(BOOT_BUTTON_GPIO);
    right.OnClick([&kb, profile]() {
        if (profile == KeyboardProfile::kProfile2) {
            ShowKeyboardTouchZoneGuide();
            return;
        }
        kb.TapKey(HID_KEY_ENTER);
    });

    kb.SetConnectionCallback([](bool c) {
        ESP_LOGI(TAG, "BLE %s", c ? "connected" : "disconnected");
    });

    ESP_LOGI(TAG,
             "keyboard app running (profile=%d, left=Right Option, boot=%s, pwr=Backspace)",
             static_cast<int>(profile),
             profile == KeyboardProfile::kProfile2 ? "Zone Guide" : "Enter");
    uint32_t heartbeat = 0;
    while (true) {
        ESP_LOGI(TAG, "keyboard heartbeat #%lu connected=%d", heartbeat++, kb.IsConnected());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
