#ifndef BLE_HID_KEYBOARD_H_
#define BLE_HID_KEYBOARD_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "mouse_move_accumulator.h"

// USB HID modifier bits / keycodes（与官方 USB HID Usage Tables 一致）
#define HID_MOD_RIGHT_ALT 0x40   // 右 Option (Right Alt/Option)
#define HID_MOD_LEFT_GUI  0x08   // 左 Command (Left GUI/Command)
#define HID_KEY_TAB       0x2B
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_ENTER     0x28
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT  0x50
#define HID_KEY_ARROW_DOWN  0x51
#define HID_KEY_ARROW_UP    0x52

constexpr uint8_t HID_MOUSE_BUTTON_LEFT = 0x01;
constexpr uint8_t HID_MOUSE_BUTTON_RIGHT = 0x02;
constexpr uint8_t HID_MOUSE_BUTTON_MIDDLE = 0x04;

enum class HidReportSendResult {
    kSent,
    kNotConnected,
    kNotReady,
    kNoBufferHeadroom,
    kNoMemory,
    kTransportError,
};

// BLE HID 键盘 + 鼠标复合设备封装（单例）。
// 键盘 report_id = 1；相对鼠标 report_id = 2。两者共用一个 BLE HID
// 连接，因此主机只需配对一次。
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

    // 累计相对鼠标位移并尝试发送一个 HID 分片。传入全零时也会继续排空
    // 先前因 BLE 拥塞而暂存的位移。
    void SendMouseMove(int8_t dx, int8_t dy, int8_t wheel = 0);

    // 切换输入模式时丢弃尚未发送的位移，避免再次进入鼠标模式时跳变。
    void CancelPendingMouseMove();

    // 按下或释放一个鼠标键；button_mask 使用 HID_MOUSE_BUTTON_*。
    void SendMouseButton(uint8_t button_mask, bool pressed);

    bool IsConnected() const { return connected_.load(); }
    bool IsAdvertising() const;
    void SetConnectionCallback(std::function<void(bool)> cb) { conn_cb_ = std::move(cb); }

    // 供 C 事件回调转发使用（内部）。
    void OnConnectChange(bool connected);
    void* dev_ = nullptr;   // esp_hidd_dev_t*

private:
    BleHidKeyboard() = default;
    BleHidKeyboard(const BleHidKeyboard&) = delete;
    BleHidKeyboard& operator=(const BleHidKeyboard&) = delete;

    void SendKeyboardReport(uint8_t report[8]);
    HidReportSendResult SendHidReport(uint8_t report_id,
                                      uint8_t* data,
                                      size_t length,
                                      int* free_msys_buffers = nullptr);
    HidReportSendResult SendMouseReport(uint8_t buttons,
                                        int8_t dx,
                                        int8_t dy,
                                        int8_t wheel,
                                        int* free_msys_buffers = nullptr);

    uint8_t report_[8] = {0};   // [modifier, reserved, key1..key6]
    uint8_t mouse_buttons_ = 0;
    bool mouse_button_report_pending_ = false;
    MouseMoveAccumulator mouse_move_accumulator_;
    uint32_t mouse_reports_sent_ = 0;
    uint32_t mouse_reports_failed_ = 0;
    uint32_t mouse_reports_throttled_ = 0;
    uint32_t mouse_moves_coalesced_ = 0;
    uint32_t mouse_pending_clamped_ = 0;
    int32_t mouse_msys_min_free_ = INT32_MAX;
    int64_t mouse_stats_last_log_us_ = 0;
    bool inited_ = false;
    std::atomic_bool connected_{false};
    std::atomic<int64_t> mouse_send_backoff_until_us_{0};
    std::function<void(bool)> conn_cb_;
};

#endif  // BLE_HID_KEYBOARD_H_
