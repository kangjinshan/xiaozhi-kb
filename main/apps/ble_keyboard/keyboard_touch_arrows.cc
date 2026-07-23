#include "keyboard_touch_arrows.h"

#include "app_mode.h"
#include "air_mouse_motion.h"
#include "ble_hid_keyboard.h"
#include "config.h"
#include "keyboard_touch_action.h"
#include "keyboard_pmic_power_key.h"
#include "keyboard_zone_display.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sensor/imu/qmi8658/SensorQMI8658.hpp>

#include <algorithm>
#include <cstdint>

#define TAG "kb_touch_arrows"

namespace {

constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kRepeatMs = 120;
constexpr uint32_t kTouchReadWarnMs = 1000;
constexpr uint32_t kPmicReadWarnMs = 1000;
constexpr uint32_t kSelectorHoldMs = 2000;
// CONFIG_FREERTOS_HZ=100, so use two full ticks. This caps mouse notifications
// at 50 Hz and leaves room for the controller/GATT confirmation path.
constexpr uint32_t kAirMousePollMs = 20;
constexpr uint32_t kAirMouseReadWarnMs = 1000;
constexpr uint8_t kAxp2101Address = 0x34;
constexpr uint8_t kAxp2101Irq1StatusReg = 0x48;
constexpr uint8_t kAxp2101Irq2EnableReg = 0x41;
constexpr uint8_t kAxp2101Irq2StatusReg = 0x49;
constexpr uint8_t kAxp2101Irq3StatusReg = 0x4A;
constexpr uint8_t kCst9217Address = 0x5A;
constexpr uint8_t kCst9217Ack = 0xAB;
constexpr uint16_t kCst9217DataReg = 0xD000;
struct TouchHardware {
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2c_master_dev_handle_t pmic = nullptr;
    i2c_master_dev_handle_t touch = nullptr;
};

struct TouchArrowContext {
    BleHidKeyboard* keyboard;
    TouchHardware hardware;
    KeyboardProfile profile;
    SemaphoreHandle_t i2c_mutex;
};

SensorQMI8658 s_air_mouse_imu;
AirMouseMotion s_air_mouse_motion;

struct KeyboardTouchSample {
    bool touched = false;
    uint16_t x = 0;
    uint16_t y = 0;
};

uint8_t HidKeyForAction(KeyboardTouchAction action) {
    switch (action) {
        case KeyboardTouchAction::kArrowUp:
            return HID_KEY_ARROW_UP;
        case KeyboardTouchAction::kArrowDown:
            return HID_KEY_ARROW_DOWN;
        case KeyboardTouchAction::kArrowLeft:
            return HID_KEY_ARROW_LEFT;
        case KeyboardTouchAction::kArrowRight:
            return HID_KEY_ARROW_RIGHT;
        case KeyboardTouchAction::kBackspace:
            return HID_KEY_BACKSPACE;
        case KeyboardTouchAction::kEnter:
            return HID_KEY_ENTER;
        default:
            return 0;
    }
}

bool IsRepeatedTapAction(KeyboardTouchAction action) {
    switch (action) {
        case KeyboardTouchAction::kArrowUp:
        case KeyboardTouchAction::kArrowDown:
        case KeyboardTouchAction::kArrowLeft:
        case KeyboardTouchAction::kArrowRight:
        case KeyboardTouchAction::kBackspace:
        case KeyboardTouchAction::kEnter:
            return true;
        default:
            return false;
    }
}

void ReleaseRightOptionIfNeeded(TouchArrowContext* context, bool* right_option_pressed) {
    if (!*right_option_pressed) {
        return;
    }
    context->keyboard->SendModifier(HID_MOD_RIGHT_ALT, false);
    *right_option_pressed = false;
}

void PressRightOptionIfNeeded(TouchArrowContext* context, bool* right_option_pressed) {
    if (*right_option_pressed) {
        return;
    }
    context->keyboard->SendModifier(HID_MOD_RIGHT_ALT, true);
    *right_option_pressed = true;
}

void ReleaseLeftCommandIfNeeded(TouchArrowContext* context, bool* left_command_pressed) {
    if (!*left_command_pressed) {
        return;
    }
    context->keyboard->SendModifier(HID_MOD_LEFT_GUI, false);
    *left_command_pressed = false;
}

void PressLeftCommandIfNeeded(TouchArrowContext* context, bool* left_command_pressed) {
    if (*left_command_pressed) {
        return;
    }
    context->keyboard->SendModifier(HID_MOD_LEFT_GUI, true);
    *left_command_pressed = true;
}

void ReleaseModifiersIfNeeded(TouchArrowContext* context,
                              bool* right_option_pressed,
                              bool* left_command_pressed) {
    ReleaseRightOptionIfNeeded(context, right_option_pressed);
    ReleaseLeftCommandIfNeeded(context, left_command_pressed);
}

const char* KeyboardProfileName(KeyboardProfile profile) {
    switch (profile) {
        case KeyboardProfile::kProfile2:
            return "profile2_keyboard_air_mouse";
        case KeyboardProfile::kProfile1:
        default:
            return "profile1";
    }
}

bool InitializeAirMouseImu(i2c_master_bus_handle_t i2c_bus) {
    if (!s_air_mouse_imu.begin(i2c_bus, QMI8658_L_SLAVE_ADDRESS)) {
        ESP_LOGE(TAG, "QMI8658 not found at 0x%02x", QMI8658_L_SLAVE_ADDRESS);
        return false;
    }

    const bool accel_configured = s_air_mouse_imu.configAccel(
        AccelFullScaleRange::FS_2G,
        224.0f,
        SensorQMI8658::LpfMode::MODE_3);
    const bool gyro_configured = s_air_mouse_imu.configGyro(
        GyroFullScaleRange::FS_1000_DPS,
        224.0f,
        SensorQMI8658::LpfMode::MODE_3);
    const bool accel_enabled = s_air_mouse_imu.enableAccel();
    const bool gyro_enabled = s_air_mouse_imu.enableGyro();
    if (!accel_configured || !gyro_configured ||
        !accel_enabled || !gyro_enabled) {
        ESP_LOGE(TAG,
                 "QMI8658 configure failed: accel_cfg=%d gyro_cfg=%d "
                 "accel_en=%d gyro_en=%d",
                 accel_configured,
                 gyro_configured,
                 accel_enabled,
                 gyro_enabled);
        return false;
    }

    // SensorLib uses accelerometer and gyro variance to refresh gyro bias only
    // while the device is stationary. This avoids a mandatory motionless boot.
    s_air_mouse_imu.enableDynamicGyroCalibration(true);
    ESP_LOGI(TAG,
             "QMI8658 air mouse ready (addr=0x%02x, odr=224Hz, task=50Hz)",
             QMI8658_L_SLAVE_ADDRESS);
    return true;
}

void AirMouseTask(void* arg) {
    auto* context = static_cast<TouchArrowContext*>(arg);
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_warning_tick = 0;
    bool warning_logged = false;

    while (true) {
        GyroscopeData gyro = {};
        xSemaphoreTake(context->i2c_mutex, portMAX_DELAY);
        const bool gyro_read = s_air_mouse_imu.readGyro(gyro);
        xSemaphoreGive(context->i2c_mutex);
        if (!gyro_read) {
            const TickType_t now = xTaskGetTickCount();
            if (!warning_logged ||
                now - last_warning_tick >= pdMS_TO_TICKS(kAirMouseReadWarnMs)) {
                ESP_LOGW(TAG, "QMI8658 gyro read failed");
                warning_logged = true;
                last_warning_tick = now;
            }
            s_air_mouse_motion.Reset();
        } else {
            if (warning_logged) {
                ESP_LOGI(TAG, "QMI8658 gyro read recovered");
                warning_logged = false;
                last_warning_tick = 0;
            }

            // Board-space mapping for holding the screen upright, facing the
            // user, while the pointing ray goes out through the back (-Z).
            // Pointing right is -Y yaw and pointing down is -X pitch. Z is
            // roll around the screen normal and must not move the cursor.
            const float horizontal_dps = gyro.dps.y;
            const float vertical_dps = gyro.dps.x;
            const bool active = context->keyboard->IsConnected();
            const AirMouseDelta delta = s_air_mouse_motion.Update(
                horizontal_dps,
                vertical_dps,
                static_cast<uint64_t>(esp_timer_get_time()),
                active);
            if (delta.HasMovement()) {
                context->keyboard->SendMouseMove(delta.x, delta.y);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kAirMousePollMs));
    }
}

esp_err_t WriteI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(device, buffer, sizeof(buffer), 100);
}

esp_err_t ReadI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(device, &reg, sizeof(reg), value, sizeof(*value), 100);
}

esp_err_t ClearAxp2101IrqStatus(i2c_master_dev_handle_t pmic) {
    esp_err_t err = WriteI2cReg(pmic, kAxp2101Irq1StatusReg, 0xFF);
    if (err != ESP_OK) {
        return err;
    }
    err = WriteI2cReg(pmic, kAxp2101Irq2StatusReg, 0xFF);
    if (err != ESP_OK) {
        return err;
    }
    return WriteI2cReg(pmic, kAxp2101Irq3StatusReg, 0xFF);
}

void CleanupTouchHardware(TouchHardware* hardware) {
    if (hardware->touch != nullptr) {
        esp_err_t err = i2c_master_bus_rm_device(hardware->touch);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "failed to remove touch device: %s",
                     esp_err_to_name(err));
        }
        hardware->touch = nullptr;
    }

    if (hardware->pmic != nullptr) {
        esp_err_t err = i2c_master_bus_rm_device(hardware->pmic);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to remove pmic device: %s", esp_err_to_name(err));
        }
        hardware->pmic = nullptr;
    }

    if (hardware->i2c_bus != nullptr) {
        esp_err_t err = i2c_del_master_bus(hardware->i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete i2c bus: %s", esp_err_to_name(err));
        }
        hardware->i2c_bus = nullptr;
    }
}

esp_err_t InitializeKeyboardTouch(TouchHardware* hardware) {
    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kCst9217Address,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    esp_err_t err =
        i2c_master_bus_add_device(hardware->i2c_bus, &device_cfg, &hardware->touch);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t reset_cfg = {};
    reset_cfg.pin_bit_mask = 1ULL << TP_RST_GPIO;
    reset_cfg.mode = GPIO_MODE_OUTPUT;
    err = gpio_config(&reset_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(TP_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_config_t interrupt_cfg = {};
    interrupt_cfg.pin_bit_mask = 1ULL << TP_INT_GPIO;
    interrupt_cfg.mode = GPIO_MODE_INPUT;
    err = gpio_config(&interrupt_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "CST9217 touch transport ready (read ACK enabled)");
    return ESP_OK;
}

esp_err_t ReadKeyboardTouch(TouchHardware* hardware,
                            KeyboardTouchSample* sample) {
    if (hardware == nullptr || hardware->touch == nullptr || sample == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample = {};

    const uint8_t reg[2] = {
        static_cast<uint8_t>(kCst9217DataReg >> 8),
        static_cast<uint8_t>(kCst9217DataReg & 0xFF),
    };
    uint8_t data[10] = {};
    esp_err_t err = i2c_master_transmit_receive(
        hardware->touch, reg, sizeof(reg), data, sizeof(data), 100);
    if (err != ESP_OK) {
        return err;
    }

    // CST9217 requires D000 + 0xAB after every successfully read frame.
    // Complete this even for idle and malformed frames so the next read is
    // not left waiting for the previous acknowledgement.
    const uint8_t ack[3] = {reg[0], reg[1], kCst9217Ack};
    err = i2c_master_transmit(hardware->touch, ack, sizeof(ack), 100);
    if (err != ESP_OK) {
        return err;
    }

    // SensorLib treats 0xAB in byte 0 as the controller's idle frame.
    if (data[0] == kCst9217Ack || data[0] == 0x00) {
        return ESP_OK;
    }
    if (data[6] != kCst9217Ack) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((data[5] & 0x7F) == 0 || (data[0] & 0x0F) != 0x06) {
        return ESP_OK;
    }

    const uint16_t raw_x =
        static_cast<uint16_t>((data[1] << 4) | (data[3] >> 4));
    const uint16_t raw_y =
        static_cast<uint16_t>((data[2] << 4) | (data[3] & 0x0F));

    // True-device calibration: the previous mapping was rotated 180 degrees.
    // Swap the raw axes, then invert X only to align all four directions and
    // corner zones with the visible guide.
    sample->x = static_cast<uint16_t>(
        (LCD_H_RES - 1) - std::min<uint16_t>(raw_y, LCD_H_RES - 1));
    sample->y = std::min<uint16_t>(raw_x, LCD_V_RES - 1);
    sample->touched = true;
    return ESP_OK;
}

esp_err_t InitializeKeyboardPmic(TouchHardware* hardware) {
    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAxp2101Address,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    esp_err_t err = i2c_master_bus_add_device(hardware->i2c_bus, &device_cfg, &hardware->pmic);
    if (err != ESP_OK) {
        return err;
    }

    const struct {
        uint8_t reg;
        uint8_t value;
    } writes[] = {
        {0x22, 0b00000110},
        {0x27, 0x10},
        {0x80, 0x01},
        {0x90, 0x00},
        {0x91, 0x00},
        {0x82, static_cast<uint8_t>((3300 - 1500) / 100)},
        {0x92, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x93, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x94, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x95, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x90, 0x0F},
        {0x64, 0x02},
        {0x61, 0x02},
        {0x62, 0x0A},
        {0x63, 0x01},
    };

    for (const auto& write : writes) {
        err = WriteI2cReg(hardware->pmic, write.reg, write.value);
        if (err != ESP_OK) {
            return err;
        }
    }

    uint8_t irq2_enable = 0;
    err = ReadI2cReg(hardware->pmic, kAxp2101Irq2EnableReg, &irq2_enable);
    if (err != ESP_OK) {
        return err;
    }
    irq2_enable |= Axp2101PowerKeyIrqEnableMask();
    err = WriteI2cReg(hardware->pmic, kAxp2101Irq2EnableReg, irq2_enable);
    if (err != ESP_OK) {
        return err;
    }

    return ClearAxp2101IrqStatus(hardware->pmic);
}

void PollPowerKeyShortcut(TouchArrowContext* context,
                          TickType_t now,
                          TickType_t* last_warning_tick,
                          bool* warning_logged,
                          bool* short_press_latched,
                          bool* display_toggle_requested,
                          uint8_t* hid_key_to_tap) {
    *hid_key_to_tap = 0;
    *display_toggle_requested = false;
    uint8_t irq2_status = 0;
    esp_err_t err = ReadI2cReg(
        context->hardware.pmic, kAxp2101Irq2StatusReg, &irq2_status);
    if (err != ESP_OK) {
        if (!*warning_logged || now - *last_warning_tick >= pdMS_TO_TICKS(kPmicReadWarnMs)) {
            ESP_LOGW(TAG, "pmic irq read failed: %s", esp_err_to_name(err));
            *last_warning_tick = now;
            *warning_logged = true;
        }
        return;
    }

    if (*warning_logged) {
        ESP_LOGI(TAG, "pmic irq read recovered");
        *warning_logged = false;
        *last_warning_tick = 0;
    }

    if (irq2_status != 0) {
        ESP_LOGI(TAG, "PWR IRQ2 status=0x%02x", irq2_status);
    }

    if (!IsAxp2101PowerKeyToggleEvent(irq2_status)) {
        *short_press_latched = false;
        if (irq2_status != 0) {
            const esp_err_t clear_err =
                ClearAxp2101IrqStatus(context->hardware.pmic);
            if (clear_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "pmic non-toggle irq clear failed: %s",
                         esp_err_to_name(clear_err));
            }
        }
        return;
    }

    esp_err_t clear_err = ClearAxp2101IrqStatus(context->hardware.pmic);
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "pmic irq clear failed: %s", esp_err_to_name(clear_err));
    }

    if (*short_press_latched) {
        return;
    }
    *short_press_latched = true;

    if (PowerKeyShortPressTogglesDisplay(context->profile)) {
        // The caller performs display/LVGL work only after releasing the
        // shared I2C lock. Initialization takes the lock internally only for
        // the three PMIC register transactions.
        *display_toggle_requested = true;
        return;
    }

    if (!context->keyboard->IsConnected()) {
        ESP_LOGI(TAG, "pwr shortcut skipped: BLE disconnected");
        return;
    }

    const uint8_t hid_key = PowerKeyShortPressHidKey(context->profile);
    ESP_LOGI(TAG, "pwr key 0x%02x", hid_key);
    *hid_key_to_tap = hid_key;
}

esp_err_t InitializeTouch(TouchHardware* hardware) {
    *hardware = {};

    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &hardware->i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    err = InitializeKeyboardPmic(hardware);
    if (err != ESP_OK) {
        CleanupTouchHardware(hardware);
        return err;
    }

    err = InitializeKeyboardTouch(hardware);
    if (err != ESP_OK) {
        CleanupTouchHardware(hardware);
        return err;
    }
    return ESP_OK;
}

void TouchArrowTask(void* arg) {
    auto* context = static_cast<TouchArrowContext*>(arg);
    KeyboardTouchAction last_tap_action = KeyboardTouchAction::kNone;
    TickType_t last_send_tick = 0;
    TickType_t last_touch_warning_tick = 0;
    bool touch_warning_logged = false;
    bool blocked_by_disconnect = false;
    bool selector_hold_logged = false;
    TickType_t selector_hold_start_tick = 0;
    TickType_t last_pmic_warning_tick = 0;
    bool pmic_warning_logged = false;
    bool power_key_short_latched = true;
    bool right_option_pressed = false;
    bool left_command_pressed = false;

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        uint8_t power_key_hid_to_tap = 0;
        bool display_toggle_requested = false;

        // Keep the PMIC + complete CST9217 read/ACK polling group atomic
        // against QMI8658 accesses on the shared I2C0 bus.
        xSemaphoreTake(context->i2c_mutex, portMAX_DELAY);
        PollPowerKeyShortcut(
            context,
            now,
            &last_pmic_warning_tick,
            &pmic_warning_logged,
            &power_key_short_latched,
            &display_toggle_requested,
            &power_key_hid_to_tap);

        KeyboardTouchAction action = KeyboardTouchAction::kNone;
        KeyboardTouchSample touch_sample = {};
        const esp_err_t touch_err =
            ReadKeyboardTouch(&context->hardware, &touch_sample);
        if (touch_err == ESP_OK && touch_sample.touched) {
            action = MapTouchPointToKeyboardAction(
                context->profile,
                touch_sample.x,
                touch_sample.y,
                LCD_H_RES,
                LCD_V_RES);
        }
        xSemaphoreGive(context->i2c_mutex);

        if (touch_err != ESP_OK) {
            if (!touch_warning_logged ||
                now - last_touch_warning_tick >=
                    pdMS_TO_TICKS(kTouchReadWarnMs)) {
                ESP_LOGW(TAG,
                         "CST9217 touch read failed: %s",
                         esp_err_to_name(touch_err));
                last_touch_warning_tick = now;
                touch_warning_logged = true;
            }
        } else if (touch_warning_logged) {
            ESP_LOGI(TAG, "CST9217 touch read recovered");
            last_touch_warning_tick = 0;
            touch_warning_logged = false;
        }

        // BLE/HID work must not hold the shared bus lock.
        if (display_toggle_requested) {
            const esp_err_t display_err = KeyboardAirMouseDisplayToggle(
                context->hardware.pmic, context->i2c_mutex);
            if (display_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "air mouse display toggle failed: %s",
                         esp_err_to_name(display_err));
            } else {
                ESP_LOGI(TAG, "air mouse display toggled");
            }
        }

        if (power_key_hid_to_tap != 0) {
            context->keyboard->TapKey(power_key_hid_to_tap);
        }

        if (action == KeyboardTouchAction::kSelector) {
            ReleaseModifiersIfNeeded(context, &right_option_pressed, &left_command_pressed);
            last_tap_action = KeyboardTouchAction::kNone;
            last_send_tick = 0;
            blocked_by_disconnect = false;

            if (selector_hold_start_tick == 0) {
                selector_hold_start_tick = now;
                selector_hold_logged = false;
            }
            if (!selector_hold_logged) {
                ESP_LOGI(TAG, "selector hot corner hold started");
                selector_hold_logged = true;
            }
            if (now - selector_hold_start_tick >= pdMS_TO_TICKS(kSelectorHoldMs)) {
                ESP_LOGI(TAG, "returning to app selector");
                AppModeWriteAndReboot(AppMode::kSelector);
            }

            vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
            continue;
        }

        selector_hold_start_tick = 0;
        selector_hold_logged = false;

        if (action == KeyboardTouchAction::kNone) {
            ReleaseModifiersIfNeeded(context, &right_option_pressed, &left_command_pressed);
            last_tap_action = KeyboardTouchAction::kNone;
            last_send_tick = 0;
            blocked_by_disconnect = false;
        } else if (action == KeyboardTouchAction::kRightOption) {
            ReleaseLeftCommandIfNeeded(context, &left_command_pressed);
            last_tap_action = KeyboardTouchAction::kNone;
            last_send_tick = 0;
            if (!context->keyboard->IsConnected()) {
                ReleaseRightOptionIfNeeded(context, &right_option_pressed);
                blocked_by_disconnect = true;
            } else {
                PressRightOptionIfNeeded(context, &right_option_pressed);
                blocked_by_disconnect = false;
            }
        } else if (action == KeyboardTouchAction::kLeftCommand) {
            ReleaseRightOptionIfNeeded(context, &right_option_pressed);
            last_tap_action = KeyboardTouchAction::kNone;
            last_send_tick = 0;
            if (!context->keyboard->IsConnected()) {
                ReleaseLeftCommandIfNeeded(context, &left_command_pressed);
                blocked_by_disconnect = true;
            } else {
                PressLeftCommandIfNeeded(context, &left_command_pressed);
                blocked_by_disconnect = false;
            }
        } else if (!context->keyboard->IsConnected()) {
            ReleaseModifiersIfNeeded(context, &right_option_pressed, &left_command_pressed);
            blocked_by_disconnect = true;
        } else if (IsRepeatedTapAction(action) &&
                   (blocked_by_disconnect ||
                   action != last_tap_action ||
                   last_send_tick == 0 ||
                   now - last_send_tick >= pdMS_TO_TICKS(kRepeatMs))) {
            ReleaseRightOptionIfNeeded(context, &right_option_pressed);
            const uint8_t hid_key = HidKeyForAction(action);
            if (hid_key != 0) {
                ESP_LOGI(TAG, "touch action %s", KeyboardTouchActionName(action));
                last_tap_action = action;
                last_send_tick = now;
                blocked_by_disconnect = false;
                ReleaseLeftCommandIfNeeded(context, &left_command_pressed);
                context->keyboard->TapKey(hid_key);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
    }
}

}  // namespace

void StartKeyboardTouchArrows(BleHidKeyboard& keyboard, KeyboardProfile profile) {
    static bool started = false;
    static TouchArrowContext context = {};

    if (started) {
        ESP_LOGW(TAG, "touch arrows already started");
        return;
    }

    TouchHardware hardware = {};
    esp_err_t err = InitializeTouch(&hardware);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch arrows disabled: %s", esp_err_to_name(err));
        return;
    }

    context.keyboard = &keyboard;
    context.hardware = hardware;
    context.profile = profile;
    context.i2c_mutex = xSemaphoreCreateMutex();
    if (context.i2c_mutex == nullptr) {
        ESP_LOGE(TAG, "failed to create shared I2C mutex");
        CleanupTouchHardware(&context.hardware);
        context = {};
        return;
    }

    if (profile == KeyboardProfile::kProfile2) {
        const esp_err_t display_err =
            KeyboardAirMouseDisplayToggle(
                context.hardware.pmic, context.i2c_mutex);
        if (display_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "initial keyboard display failed: %s",
                     esp_err_to_name(display_err));
        } else {
            ESP_LOGI(TAG, "initial keyboard display on");
        }
    }

    bool air_mouse_ready = false;
    if (profile == KeyboardProfile::kProfile2) {
        xSemaphoreTake(context.i2c_mutex, portMAX_DELAY);
        air_mouse_ready = InitializeAirMouseImu(context.hardware.i2c_bus);
        xSemaphoreGive(context.i2c_mutex);
    }

    BaseType_t task_ok = xTaskCreate(
        TouchArrowTask, "touch_arrows", 4096, &context, 5, nullptr);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch arrow task");
        vSemaphoreDelete(context.i2c_mutex);
        context.i2c_mutex = nullptr;
        CleanupTouchHardware(&context.hardware);
        context = {};
        return;
    }

    started = true;
    if (air_mouse_ready) {
        BaseType_t air_mouse_task_ok = xTaskCreate(
            AirMouseTask, "air_mouse", 4096, &context, 6, nullptr);
        if (air_mouse_task_ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create air mouse task");
        }
    } else if (profile == KeyboardProfile::kProfile2) {
        ESP_LOGW(TAG,
                 "air mouse disabled; BLE keyboard and touch selector remain active");
    }
    ESP_LOGI(TAG, "touch actions started (%s)", KeyboardProfileName(profile));
}
