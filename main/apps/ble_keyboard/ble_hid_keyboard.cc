#include "ble_hid_keyboard.h"

#include <algorithm>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "esp_hidd.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/hid/ble_svc_hid.h"
#else
#include "esp_gatts_api.h"
#include "esp_hidd_gatts.h"
#endif
#include "esp_hid_gap.h"
#include "hid_notification_flow_control.h"

#define TAG "ble_hid_kb"

namespace {

constexpr int64_t kMouseSendBackoffUs = 100 * 1000;
constexpr int64_t kMouseMsysRetryBackoffUs = 20 * 1000;
constexpr int64_t kMouseStatsLogIntervalUs = 10 * 1000 * 1000;

#if CONFIG_BT_NIMBLE_ENABLED
constexpr int kNimbleSmallPoolReserve = 8;

std::atomic<uint16_t> s_nimble_conn_handle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint8_t> s_nimble_protocol_mode{ESP_HID_PROTOCOL_MODE_REPORT};
std::atomic<uint16_t> s_keyboard_report_handle{0};
std::atomic<uint16_t> s_mouse_report_handle{0};
std::atomic<uint16_t> s_boot_keyboard_report_handle{0};
std::atomic<uint16_t> s_boot_mouse_report_handle{0};
std::atomic<int> s_nimble_msys_high_watermark{0};
ble_gatt_register_fn* s_hidd_gatts_register_cb = nullptr;
void* s_hidd_gatts_register_arg = nullptr;
int s_input_report_registration_index = 0;

void CaptureHidGattHandle(struct ble_gatt_register_ctxt* ctxt, void*) {
    if (s_hidd_gatts_register_cb != nullptr) {
        s_hidd_gatts_register_cb(ctxt, s_hidd_gatts_register_arg);
    }

    if (ctxt == nullptr || ctxt->op != BLE_GATT_REGISTER_OP_CHR ||
        ctxt->chr.chr_def == nullptr || ctxt->chr.svc_def == nullptr ||
        ble_uuid_u16(ctxt->chr.svc_def->uuid) != BLE_SVC_HID_UUID16) {
        return;
    }

    const uint16_t uuid = ble_uuid_u16(ctxt->chr.chr_def->uuid);
    if (uuid == BLE_SVC_HID_CHR_UUID16_RPT &&
        (ctxt->chr.chr_def->flags & BLE_GATT_CHR_F_NOTIFY) != 0) {
        if (s_input_report_registration_index == 0) {
            s_keyboard_report_handle.store(ctxt->chr.val_handle);
            ESP_LOGI(TAG,
                     "captured keyboard report handle=%u",
                     ctxt->chr.val_handle);
        } else if (s_input_report_registration_index == 1) {
            s_mouse_report_handle.store(ctxt->chr.val_handle);
            ESP_LOGI(TAG,
                     "captured mouse report handle=%u",
                     ctxt->chr.val_handle);
        }
        ++s_input_report_registration_index;
    } else if (uuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP) {
        s_boot_keyboard_report_handle.store(ctxt->chr.val_handle);
    } else if (uuid == BLE_SVC_HID_CHR_UUID16_BOOT_MOUSE_INP) {
        s_boot_mouse_report_handle.store(ctxt->chr.val_handle);
    }
}

void InstallHidGattHandleCapture() {
    s_keyboard_report_handle.store(0);
    s_mouse_report_handle.store(0);
    s_boot_keyboard_report_handle.store(0);
    s_boot_mouse_report_handle.store(0);
    s_input_report_registration_index = 0;
    s_hidd_gatts_register_cb = ble_hs_cfg.gatts_register_cb;
    s_hidd_gatts_register_arg = ble_hs_cfg.gatts_register_arg;
    ble_hs_cfg.gatts_register_cb = CaptureHidGattHandle;
    ble_hs_cfg.gatts_register_arg = nullptr;
}
#endif

}  // namespace

// 官方 esp_hid_device 示例的 GAP 处理（esp_hid_gap.c）在 BLE 认证完成事件里会调用
// ble_hid_task_start_up() 启动示例自带的发送线程。该函数原本定义在示例 main.c 中，
// 拷贝 GAP 文件时没有一并带入，全工程无定义。本封装改用 HID 事件回调跟踪连接，不需要
// 那套发送线程，因此在这里补一个空桩，避免 Task 6 调用 Init() 拉入 GAP 链后出现
// undefined reference to `ble_hid_task_start_up`。
extern "C" void ble_hid_task_start_up(void) {}

#if CONFIG_BT_NIMBLE_ENABLED
extern "C" void ble_hid_transport_gap_connection_changed(
    uint16_t conn_handle, bool connected) {
    s_nimble_conn_handle.store(
        connected ? conn_handle : BLE_HS_CONN_HANDLE_NONE);
    s_nimble_msys_high_watermark.store(0);
    if (!connected) {
        s_nimble_protocol_mode.store(ESP_HID_PROTOCOL_MODE_REPORT);
    }
}
#endif

#if CONFIG_BT_NIMBLE_ENABLED
extern "C" void ble_store_config_init(void);

static void BleHidHostTask(void*) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

// 键盘（Report ID 1）和相对鼠标（Report ID 2）共用一个 HID report map。
// 键盘输入 8 字节：[modifier, reserved, key1..key6]；鼠标输入 4 字节：
// [buttons, dx, dy, wheel]。
static const uint8_t kKeyboardMouseReportMap[] = {
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

    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)
    0xC0,              //   End Collection
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = kKeyboardMouseReportMap,
        .len = sizeof(kKeyboardMouseReportMap),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0101,
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

static esp_err_t LogInitStep(const char* step, esp_err_t err) {
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DIAG %s OK", step);
    } else {
        ESP_LOGE(TAG, "DIAG %s failed: %s", step, esp_err_to_name(err));
    }
    return err;
}

class ReportLock {
public:
    ReportLock() : mutex_(GetReportMutex()) {
        if (mutex_ != nullptr) {
            locked_ = xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
        }
    }

    ~ReportLock() {
        if (locked_) {
            xSemaphoreGive(mutex_);
        }
    }

    bool locked() const { return locked_; }

private:
    static SemaphoreHandle_t GetReportMutex() {
        static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
        if (mutex == nullptr) {
            ESP_LOGE(TAG, "failed to create report mutex");
        }
        return mutex;
    }

    SemaphoreHandle_t mutex_ = nullptr;
    bool locked_ = false;
};

// esp_hidd 设备事件回调（C 事件循环回调，转发给单例）。
static void hidd_event_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    (void)handler_args;
    (void)base;
    auto* param = static_cast<esp_hidd_event_data_t*>(event_data);
    switch (static_cast<esp_hidd_event_t>(id)) {
        case ESP_HIDD_START_EVENT:
            ESP_LOGI(TAG, "HID START, begin advertising");
            if (const esp_err_t err = esp_hid_ble_gap_adv_start();
                err != ESP_OK) {
                ESP_LOGE(TAG, "BLE advertising start failed: %d", err);
            }
            break;
        case ESP_HIDD_CONNECT_EVENT:
            ESP_LOGI(TAG, "HID CONNECT");
            BleHidKeyboard::GetInstance().OnConnectChange(true);
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            ESP_LOGI(TAG, "HID DISCONNECT");
            BleHidKeyboard::GetInstance().OnConnectChange(false);
            // 断开后重新广播，方便主机重新连接。
            if (const esp_err_t err = esp_hid_ble_gap_adv_start();
                err != ESP_OK) {
                ESP_LOGE(TAG, "BLE advertising restart failed: %d", err);
            }
            break;
#if CONFIG_BT_NIMBLE_ENABLED
        case ESP_HIDD_PROTOCOL_MODE_EVENT:
            s_nimble_protocol_mode.store(param->protocol_mode.protocol_mode);
            ESP_LOGI(TAG,
                     "HID protocol mode=%u",
                     static_cast<unsigned>(param->protocol_mode.protocol_mode));
            break;
#endif
        default:
            break;
    }
    (void)param;
}

void BleHidKeyboard::OnConnectChange(bool connected) {
    {
        ReportLock lock;
        if (lock.locked()) {
            // A held physical key cannot be reconstructed across reconnects.
            // Start every transport session from a neutral report instead of
            // replaying state accumulated while disconnected.
            std::memset(report_, 0, sizeof(report_));
            mouse_buttons_ = 0;
            // After a new connection, send one neutral mouse state through
            // the normal flow-controlled path. This clears any host-side
            // button state and verifies that the captured report handle is
            // usable even before the first physical movement.
            mouse_button_report_pending_ = connected;
            mouse_move_accumulator_.Reset();
            mouse_reports_sent_ = 0;
            mouse_reports_failed_ = 0;
            mouse_reports_throttled_ = 0;
            mouse_moves_coalesced_ = 0;
            mouse_pending_clamped_ = 0;
            mouse_msys_min_free_ = INT32_MAX;
            mouse_stats_last_log_us_ = 0;
        }
    }
    mouse_send_backoff_until_us_.store(0);
    connected_.store(connected);
    if (conn_cb_) {
        conn_cb_(connected);
    }
}

bool BleHidKeyboard::IsAdvertising() const {
#if CONFIG_BT_NIMBLE_ENABLED
    return ble_gap_adv_active() != 0;
#else
    return !connected_.load();
#endif
}

void BleHidKeyboard::Init() {
    if (inited_) {
        ESP_LOGW(TAG, "already inited");
        return;
    }

    // 1) GAP 初始化：BLE-only（C6）。esp_hid_gap_init 内部初始化 BLE 控制器 + Bluedroid，
    //    并注册 BLE GAP 回调。传 HIDD_BLE_MODE(=ESP_BT_MODE_BLE=0x01)。
    ESP_ERROR_CHECK(LogInitStep("esp_hid_gap_init", esp_hid_gap_init(HIDD_BLE_MODE)));

    // 2) 广播 + Just Works 安全参数（iocap = ESP_IO_CAP_NONE，见 esp_hid_gap.c 内已改）。
    ESP_ERROR_CHECK(LogInitStep("esp_hid_ble_gap_adv_init", esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, s_hid_config.device_name)));

    // 3) Bluedroid needs the global GATTS callback registered before dev_init.
    // NimBLE's esp_hid backend registers its own GATT server callbacks.
#if !CONFIG_BT_NIMBLE_ENABLED
    ESP_ERROR_CHECK(LogInitStep(
        "esp_ble_gatts_register_callback",
        esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)));
#endif

    // 4) 初始化 HID 设备，transport = BLE。
    esp_hidd_dev_t* dev = nullptr;
    ESP_ERROR_CHECK(LogInitStep("esp_hidd_dev_init", esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_cb, &dev)));
    dev_ = dev;

#if CONFIG_BT_NIMBLE_ENABLED
    // Capture the HID input characteristic handles while NimBLE registers the
    // service. Sending directly to these handles lets us handle mbuf
    // exhaustion as normal backpressure instead of entering esp_hid's
    // assert(ble_hs_mbuf_from_flat(...) != nullptr) path.
    InstallHidGattHandleCapture();

    // esp_nimble_init() only initializes the port. The host task must start
    // after the HID service installs ble_hs_cfg callbacks; otherwise the
    // sync callback and ESP_HIDD_START_EVENT never run, so no advertising is
    // emitted even though the application itself remains healthy.
    ESP_ERROR_CHECK(LogInitStep(
        "ble_svc_gap_device_name_set",
        static_cast<esp_err_t>(
            ble_svc_gap_device_name_set(s_hid_config.device_name))));
    ESP_ERROR_CHECK(LogInitStep(
        "ble_svc_gap_device_appearance_set",
        static_cast<esp_err_t>(
            ble_svc_gap_device_appearance_set(
                ESP_HID_APPEARANCE_KEYBOARD))));
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ESP_ERROR_CHECK(LogInitStep(
        "esp_nimble_enable",
        esp_nimble_enable(reinterpret_cast<void*>(BleHidHostTask))));
#endif

    inited_ = true;
    ESP_LOGI(TAG,
             "BLE HID keyboard+mouse '%s' inited (Just Works, no PIN)",
             s_hid_config.device_name);
}

void BleHidKeyboard::SendKeyboardReport(uint8_t report[8]) {
    (void)SendHidReport(1, report, 8);
}

HidReportSendResult BleHidKeyboard::SendHidReport(
    uint8_t report_id,
    uint8_t* data,
    size_t length,
    int* free_msys_buffers) {
    if (dev_ == nullptr || !connected_.load()) {
        return HidReportSendResult::kNotConnected;
    }

#if CONFIG_BT_NIMBLE_ENABLED
    const uint16_t conn_handle = s_nimble_conn_handle.load();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return HidReportSendResult::kNotConnected;
    }

    const int free_buffers = os_msys_num_free();
    if (free_msys_buffers != nullptr) {
        *free_msys_buffers = free_buffers;
    }
    int high_watermark = s_nimble_msys_high_watermark.load();
    while (free_buffers > high_watermark &&
           !s_nimble_msys_high_watermark.compare_exchange_weak(
               high_watermark, free_buffers)) {
    }
    high_watermark = std::max(high_watermark, free_buffers);
    if (!HidNotificationHasBufferHeadroom(
            free_buffers,
            high_watermark,
            CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT,
            kNimbleSmallPoolReserve)) {
        return HidReportSendResult::kNoBufferHeadroom;
    }

    uint16_t attr_handle = 0;
    size_t notification_length = length;
    if (s_nimble_protocol_mode.load() == ESP_HID_PROTOCOL_MODE_BOOT) {
        if (report_id == 1) {
            attr_handle = s_boot_keyboard_report_handle.load();
        } else if (report_id == 2) {
            attr_handle = s_boot_mouse_report_handle.load();
            notification_length = std::min<size_t>(notification_length, 3);
        }
    } else if (report_id == 1) {
        attr_handle = s_keyboard_report_handle.load();
    } else if (report_id == 2) {
        attr_handle = s_mouse_report_handle.load();
    }
    if (attr_handle == 0) {
        return HidReportSendResult::kNotReady;
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, notification_length);
    if (om == nullptr) {
        return HidReportSendResult::kNoMemory;
    }
    const int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    return rc == 0 ? HidReportSendResult::kSent
                   : HidReportSendResult::kTransportError;
#else
    return esp_hidd_dev_input_set(
               static_cast<esp_hidd_dev_t*>(dev_),
               0,
               report_id,
               data,
               length) == ESP_OK
               ? HidReportSendResult::kSent
               : HidReportSendResult::kTransportError;
#endif
}

void BleHidKeyboard::SendModifier(uint8_t modifier_bits, bool pressed) {
    uint8_t report[sizeof(report_)] = {};
    {
        ReportLock lock;
        if (!lock.locked()) {
            return;
        }

        if (pressed) {
            report_[0] |= modifier_bits;
        } else {
            report_[0] &= static_cast<uint8_t>(~modifier_bits);
        }
        std::memcpy(report, report_, sizeof(report));
    }
    SendKeyboardReport(report);
}

void BleHidKeyboard::TapKey(uint8_t keycode) {
    static SemaphoreHandle_t tap_mutex = xSemaphoreCreateMutex();
    if (tap_mutex == nullptr ||
        xSemaphoreTake(tap_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    uint8_t press_report[sizeof(report_)] = {};
    {
        ReportLock lock;
        if (lock.locked()) {
            report_[2] = keycode;
            std::memcpy(press_report, report_, sizeof(press_report));
        }
    }
    SendKeyboardReport(press_report);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t release_report[sizeof(report_)] = {};
    {
        ReportLock lock;
        if (lock.locked()) {
            report_[2] = 0;
            std::memcpy(release_report, report_, sizeof(release_report));
        }
    }
    SendKeyboardReport(release_report);
    xSemaphoreGive(tap_mutex);
}

void BleHidKeyboard::SendMouseMove(int8_t dx, int8_t dy, int8_t wheel) {
    const int64_t now_us = esp_timer_get_time();
    bool log_stats = false;
    uint32_t reports_sent = 0;
    uint32_t reports_failed = 0;
    uint32_t reports_throttled = 0;
    uint32_t moves_coalesced = 0;
    uint32_t pending_clamped = 0;
    int32_t pending_x = 0;
    int32_t pending_y = 0;
    int32_t msys_min_free = -1;
    {
        ReportLock lock;
        if (!lock.locked()) {
            return;
        }

        if (!connected_.load()) {
            mouse_move_accumulator_.Reset();
            return;
        }

        const bool has_new_movement = dx != 0 || dy != 0 || wheel != 0;
        if (has_new_movement) {
            if (mouse_move_accumulator_.HasPending()) {
                ++mouse_moves_coalesced_;
            }
            if (mouse_move_accumulator_.Add(dx, dy, wheel)) {
                ++mouse_pending_clamped_;
            }
        }

        if (now_us >= mouse_send_backoff_until_us_.load()) {
            const MouseMoveChunk chunk = mouse_move_accumulator_.PeekChunk();
            if (chunk.HasMovement() || mouse_button_report_pending_) {
                int free_buffers = -1;
                const HidReportSendResult result = SendMouseReport(
                    mouse_buttons_,
                    chunk.x,
                    chunk.y,
                    chunk.wheel,
                    &free_buffers);
                if (free_buffers >= 0) {
                    mouse_msys_min_free_ = std::min<int32_t>(
                        mouse_msys_min_free_, free_buffers);
                }
                if (result == HidReportSendResult::kSent) {
                    mouse_move_accumulator_.Commit(chunk);
                    mouse_button_report_pending_ = false;
                    ++mouse_reports_sent_;
                } else if (
                    result == HidReportSendResult::kNoBufferHeadroom ||
                    result == HidReportSendResult::kNoMemory) {
                    ++mouse_reports_throttled_;
                    mouse_send_backoff_until_us_.store(
                        now_us + kMouseMsysRetryBackoffUs);
                } else {
                    // Keep the complete movement pending. Future 20 ms touch
                    // polls continue coalescing while the transport gets a
                    // bounded quiet period instead of dropping cursor travel.
                    ++mouse_reports_failed_;
                    mouse_send_backoff_until_us_.store(
                        now_us + kMouseSendBackoffUs);
                }
            }
        }

        const bool has_activity = mouse_reports_sent_ != 0 ||
                                  mouse_reports_failed_ != 0 ||
                                  mouse_reports_throttled_ != 0 ||
                                  mouse_move_accumulator_.HasPending();
        if (has_activity &&
            now_us - mouse_stats_last_log_us_ >=
                kMouseStatsLogIntervalUs) {
            mouse_stats_last_log_us_ = now_us;
            log_stats = true;
            reports_sent = mouse_reports_sent_;
            reports_failed = mouse_reports_failed_;
            reports_throttled = mouse_reports_throttled_;
            moves_coalesced = mouse_moves_coalesced_;
            pending_clamped = mouse_pending_clamped_;
            pending_x = mouse_move_accumulator_.pending_x();
            pending_y = mouse_move_accumulator_.pending_y();
            msys_min_free = mouse_msys_min_free_ == INT32_MAX
                                 ? -1
                                 : mouse_msys_min_free_;
        }
    }

    if (log_stats) {
        ESP_LOGI(TAG,
                 "mouse transport sent=%lu failed=%lu throttled=%lu "
                 "coalesced=%lu clamped=%lu pending=(%ld,%ld) "
                 "msys_min_free=%ld",
                 static_cast<unsigned long>(reports_sent),
                 static_cast<unsigned long>(reports_failed),
                 static_cast<unsigned long>(reports_throttled),
                 static_cast<unsigned long>(moves_coalesced),
                 static_cast<unsigned long>(pending_clamped),
                 static_cast<long>(pending_x),
                 static_cast<long>(pending_y),
                 static_cast<long>(msys_min_free));
    }
}

void BleHidKeyboard::CancelPendingMouseMove() {
    ReportLock lock;
    if (!lock.locked()) {
        return;
    }
    mouse_move_accumulator_.Reset();
    mouse_send_backoff_until_us_.store(0);
}

void BleHidKeyboard::SendMouseButton(uint8_t button_mask, bool pressed) {
    {
        ReportLock lock;
        if (!lock.locked()) {
            return;
        }

        if (pressed) {
            mouse_buttons_ |= button_mask;
        } else {
            mouse_buttons_ &= static_cast<uint8_t>(~button_mask);
        }
        mouse_button_report_pending_ = true;
    }
    SendMouseMove(0, 0, 0);
}

HidReportSendResult BleHidKeyboard::SendMouseReport(
    uint8_t buttons,
    int8_t dx,
    int8_t dy,
    int8_t wheel,
    int* free_msys_buffers) {
    uint8_t mouse_report[4] = {
        buttons,
        static_cast<uint8_t>(dx),
        static_cast<uint8_t>(dy),
        static_cast<uint8_t>(wheel),
    };
    return SendHidReport(
        2, mouse_report, sizeof(mouse_report), free_msys_buffers);
}
