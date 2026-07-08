#include "ble_hid_keyboard.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_gap.h"

#define TAG "ble_hid_kb"

// 官方 esp_hid_device 示例的 GAP 处理（esp_hid_gap.c）在 BLE 认证完成事件里会调用
// ble_hid_task_start_up() 启动示例自带的发送线程。该函数原本定义在示例 main.c 中，
// 拷贝 GAP 文件时没有一并带入，全工程无定义。本封装改用 HID 事件回调跟踪连接，不需要
// 那套发送线程，因此在这里补一个空桩，避免 Task 6 调用 Init() 拉入 GAP 链后出现
// undefined reference to `ble_hid_task_start_up`。
extern "C" void ble_hid_task_start_up(void) {}

// 标准键盘 report map（report_id = 1）。
// 输入 8 字节：[modifier, reserved, key1..key6]；输出 1 字节（LED）。
// 从 docs/superpowers/reference/esp_hid_device_main.c 的 keyboardReportMap 原样移植（65 字节）。
static const uint8_t kKeyboardReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)  -> modifier byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs) -> reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs) -> LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs) -> LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array) -> 6 key codes
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = kKeyboardReportMap,
        .len = sizeof(kKeyboardReportMap),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = "XiaoZhi KB",
    .manufacturer_name = "XiaoZhi",
    .serial_number = "000001",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

BleHidKeyboard& BleHidKeyboard::GetInstance() {
    static BleHidKeyboard inst;
    return inst;
}

// esp_hidd 设备事件回调（C 事件循环回调，转发给单例）。
static void hidd_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    (void)handler_args;
    (void)base;
    auto* param = static_cast<esp_hidd_event_data_t*>(event_data);
    switch (static_cast<esp_hidd_event_t>(id)) {
        case ESP_HIDD_START_EVENT:
            ESP_LOGI(TAG, "HID START, begin advertising");
            esp_hid_ble_gap_adv_start();
            break;
        case ESP_HIDD_CONNECT_EVENT:
            ESP_LOGI(TAG, "HID CONNECT");
            BleHidKeyboard::GetInstance().OnConnectChange(true);
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            ESP_LOGI(TAG, "HID DISCONNECT");
            BleHidKeyboard::GetInstance().OnConnectChange(false);
            // 断开后重新广播，方便主机重新连接。
            esp_hid_ble_gap_adv_start();
            break;
        default:
            break;
    }
    (void)param;
}

void BleHidKeyboard::OnConnectChange(bool connected) {
    connected_ = connected;
    if (conn_cb_) {
        conn_cb_(connected);
    }
}

void BleHidKeyboard::Init() {
    if (inited_) {
        ESP_LOGW(TAG, "already inited");
        return;
    }

    // 1) GAP 初始化：BLE-only（C6）。esp_hid_gap_init 内部初始化 BLE 控制器 + Bluedroid，
    //    并注册 BLE GAP 回调。传 HIDD_BLE_MODE(=ESP_BT_MODE_BLE=0x01)。
    ESP_ERROR_CHECK(esp_hid_gap_init(HIDD_BLE_MODE));

    // 2) 广播 + Just Works 安全参数（iocap = ESP_IO_CAP_NONE，见 esp_hid_gap.c 内已改）。
    ESP_ERROR_CHECK(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, s_hid_config.device_name));

    // 3) 注册 HID GATTS 事件回调（必须在 dev_init 之前，Bluedroid 要求）。
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler));

    // 4) 初始化 HID 设备，transport = BLE。
    esp_hidd_dev_t* dev = nullptr;
    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &dev));
    dev_ = dev;

    inited_ = true;
    ESP_LOGI(TAG, "BLE HID keyboard '%s' inited (Just Works, no PIN)", s_hid_config.device_name);
}

void BleHidKeyboard::SendReport() {
    if (dev_ == nullptr) {
        return;
    }
    // map_index = 0, report_id = 1, 8 字节标准键盘 report。
    esp_hidd_dev_input_set(static_cast<esp_hidd_dev_t*>(dev_), 0, 1, report_, sizeof(report_));
}

void BleHidKeyboard::SendModifier(uint8_t modifier_bits, bool pressed) {
    if (pressed) {
        report_[0] |= modifier_bits;
    } else {
        report_[0] &= static_cast<uint8_t>(~modifier_bits);
    }
    SendReport();
}

void BleHidKeyboard::TapKey(uint8_t keycode) {
    report_[2] = keycode;
    SendReport();
    vTaskDelay(pdMS_TO_TICKS(50));
    report_[2] = 0;
    SendReport();
}
