#ifndef BLE_HID_KEYBOARD_H_
#define BLE_HID_KEYBOARD_H_

#include <cstdint>
#include <functional>

// USB HID modifier bits / keycodes（与官方 USB HID Usage Tables 一致）
#define HID_MOD_RIGHT_ALT 0x40   // 右 Option (Right Alt/Option)
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_ENTER     0x28

// BLE HID 标准键盘封装（单例）。
// 作为标准 BLE HID 键盘被主机（MacBook 等）以 Just Works（无 PIN）方式配对，
// 并发送标准 8 字节键盘 report：[modifier, reserved, key1..key6]，report_id = 1。
class BleHidKeyboard {
public:
    static BleHidKeyboard& GetInstance();

    // 初始化 GAP（Just Works 安全参数）+ HID 设备 + 广播。
    // 必须在 nvs_flash_init() 之后调用。
    void Init();

    // 修饰键按住/释放：pressed 时在 report[0] 置位，release 时清位，然后发送。
    void SendModifier(uint8_t modifier_bits, bool pressed);

    // 敲一下普通键：report[2]=keycode 发送 -> 50ms -> 全 0 发送。
    void TapKey(uint8_t keycode);

    bool IsConnected() const { return connected_; }
    void SetConnectionCallback(std::function<void(bool)> cb) { conn_cb_ = std::move(cb); }

    // 供 C 事件回调转发使用（内部）。
    void OnConnectChange(bool connected);
    void* dev_ = nullptr;   // esp_hidd_dev_t*

private:
    BleHidKeyboard() = default;
    BleHidKeyboard(const BleHidKeyboard&) = delete;
    BleHidKeyboard& operator=(const BleHidKeyboard&) = delete;

    void SendReport();

    uint8_t report_[8] = {0};   // [modifier, reserved, key1..key6]
    bool inited_ = false;
    bool connected_ = false;
    std::function<void(bool)> conn_cb_;
};

#endif  // BLE_HID_KEYBOARD_H_
