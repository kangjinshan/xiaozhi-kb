#include "keyboard_touch_arrows.h"

#include "ble_hid_keyboard.h"
#include "config.h"
#include "touch_arrow_mapper.h"

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

#define TAG "kb_touch_arrows"

namespace {

constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kRepeatMs = 120;
constexpr uint32_t kTouchReadWarnMs = 1000;
constexpr uint8_t kAxp2101Address = 0x34;

struct TouchHardware {
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2c_master_dev_handle_t pmic = nullptr;
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_lcd_touch_handle_t touch = nullptr;
};

struct TouchArrowContext {
    BleHidKeyboard* keyboard;
    TouchHardware hardware;
};

uint8_t HidKeyForDirection(TouchArrowDirection direction) {
    switch (direction) {
        case TouchArrowDirection::kUp:
            return HID_KEY_ARROW_UP;
        case TouchArrowDirection::kDown:
            return HID_KEY_ARROW_DOWN;
        case TouchArrowDirection::kLeft:
            return HID_KEY_ARROW_LEFT;
        case TouchArrowDirection::kRight:
            return HID_KEY_ARROW_RIGHT;
        case TouchArrowDirection::kNone:
        default:
            return 0;
    }
}

esp_err_t WriteI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(device, buffer, sizeof(buffer), 100);
}

void CleanupTouchHardware(TouchHardware* hardware) {
    if (hardware->touch != nullptr) {
        esp_err_t err = esp_lcd_touch_del(hardware->touch);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete touch handle: %s", esp_err_to_name(err));
        }
        hardware->touch = nullptr;
    }

    if (hardware->touch_io != nullptr) {
        esp_err_t err = esp_lcd_panel_io_del(hardware->touch_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to delete touch io: %s", esp_err_to_name(err));
        }
        hardware->touch_io = nullptr;
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

    return ESP_OK;
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

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    touch_io_config.scl_speed_hz = 400 * 1000;
    err = esp_lcd_new_panel_io_i2c(hardware->i2c_bus, &touch_io_config, &hardware->touch_io);
    if (err != ESP_OK) {
        CleanupTouchHardware(hardware);
        return err;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES - 1,
        .y_max = LCD_V_RES - 1,
        .rst_gpio_num = TP_RST_GPIO,
        .int_gpio_num = TP_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    err = esp_lcd_touch_new_i2c_cst9217(hardware->touch_io, &touch_cfg, &hardware->touch);
    if (err != ESP_OK) {
        CleanupTouchHardware(hardware);
    }

    return err;
}

void TouchArrowTask(void* arg) {
    auto* context = static_cast<TouchArrowContext*>(arg);
    TouchArrowDirection last_direction = TouchArrowDirection::kNone;
    TickType_t last_send_tick = 0;
    TickType_t last_read_warning_tick = 0;
    bool touch_read_failed = false;
    bool touch_read_warning_logged = false;
    bool blocked_by_disconnect = false;

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        TouchArrowDirection direction = TouchArrowDirection::kNone;
        esp_err_t err = esp_lcd_touch_read_data(context->hardware.touch);
        if (err == ESP_OK) {
            esp_lcd_touch_point_data_t touch_point = {};
            uint8_t touch_count = 0;
            err = esp_lcd_touch_get_data(context->hardware.touch, &touch_point, &touch_count, 1);
            if (err == ESP_OK) {
                if (touch_read_failed) {
                    ESP_LOGI(TAG, "touch read recovered");
                    touch_read_failed = false;
                    last_read_warning_tick = 0;
                    touch_read_warning_logged = false;
                }
                if (touch_count > 0) {
                    direction = MapTouchPointToArrow(touch_point.x, touch_point.y, LCD_H_RES, LCD_V_RES);
                }
            } else if (err != ESP_OK) {
                touch_read_failed = true;
                if (!touch_read_warning_logged ||
                    now - last_read_warning_tick >= pdMS_TO_TICKS(kTouchReadWarnMs)) {
                    ESP_LOGW(TAG, "touch data failed: %s", esp_err_to_name(err));
                    last_read_warning_tick = now;
                    touch_read_warning_logged = true;
                }
            }
        } else {
            touch_read_failed = true;
            if (!touch_read_warning_logged ||
                now - last_read_warning_tick >= pdMS_TO_TICKS(kTouchReadWarnMs)) {
                ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
                last_read_warning_tick = now;
                touch_read_warning_logged = true;
            }
        }

        if (direction == TouchArrowDirection::kNone) {
            last_direction = TouchArrowDirection::kNone;
            last_send_tick = 0;
            blocked_by_disconnect = false;
        } else if (!context->keyboard->IsConnected()) {
            blocked_by_disconnect = true;
        } else if (blocked_by_disconnect ||
                   direction != last_direction ||
                   last_send_tick == 0 ||
                   now - last_send_tick >= pdMS_TO_TICKS(kRepeatMs)) {
            const uint8_t hid_key = HidKeyForDirection(direction);
            if (hid_key != 0) {
                ESP_LOGI(TAG, "touch arrow %s", TouchArrowDirectionName(direction));
                last_direction = direction;
                last_send_tick = now;
                blocked_by_disconnect = false;
                context->keyboard->TapKey(hid_key);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
    }
}

}  // namespace

void StartKeyboardTouchArrows(BleHidKeyboard& keyboard) {
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

    BaseType_t task_ok = xTaskCreate(
        TouchArrowTask, "touch_arrows", 4096, &context, 5, nullptr);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch arrow task");
        CleanupTouchHardware(&context.hardware);
        context = {};
        return;
    }

    started = true;
    ESP_LOGI(TAG, "touch arrows started");
}
