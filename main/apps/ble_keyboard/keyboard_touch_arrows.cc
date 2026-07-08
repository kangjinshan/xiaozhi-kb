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

#define TAG "kb_touch_arrows"

namespace {

constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kRepeatMs = 120;
constexpr uint8_t kAxp2101Address = 0x34;

struct TouchArrowContext {
    BleHidKeyboard* keyboard;
    esp_lcd_touch_handle_t touch;
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

esp_err_t InitializeKeyboardPmic(i2c_master_bus_handle_t i2c_bus) {
    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAxp2101Address,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    i2c_master_dev_handle_t pmic = nullptr;
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &device_cfg, &pmic);
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
        err = WriteI2cReg(pmic, write.reg, write.value);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t InitializeTouch(esp_lcd_touch_handle_t* out_touch) {
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

    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    err = InitializeKeyboardPmic(i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    touch_io_config.scl_speed_hz = 400 * 1000;
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_config, &touch_io);
    if (err != ESP_OK) {
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

    return esp_lcd_touch_new_i2c_cst9217(touch_io, &touch_cfg, out_touch);
}

void TouchArrowTask(void* arg) {
    auto* context = static_cast<TouchArrowContext*>(arg);
    TouchArrowDirection last_direction = TouchArrowDirection::kNone;
    TickType_t last_send_tick = 0;

    while (true) {
        TouchArrowDirection direction = TouchArrowDirection::kNone;
        esp_err_t err = esp_lcd_touch_read_data(context->touch);
        if (err == ESP_OK) {
            uint16_t x = 0;
            uint16_t y = 0;
            uint8_t touch_count = 0;
            const bool pressed = esp_lcd_touch_get_coordinates(
                context->touch, &x, &y, nullptr, &touch_count, 1);
            if (pressed && touch_count > 0) {
                direction = MapTouchPointToArrow(x, y, LCD_H_RES, LCD_V_RES);
            }
        } else {
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
        }

        const TickType_t now = xTaskGetTickCount();
        if (direction == TouchArrowDirection::kNone) {
            last_direction = TouchArrowDirection::kNone;
            last_send_tick = 0;
        } else if (direction != last_direction ||
                   last_send_tick == 0 ||
                   now - last_send_tick >= pdMS_TO_TICKS(kRepeatMs)) {
            const uint8_t hid_key = HidKeyForDirection(direction);
            if (hid_key != 0 && context->keyboard->IsConnected()) {
                ESP_LOGI(TAG, "touch arrow %s", TouchArrowDirectionName(direction));
                context->keyboard->TapKey(hid_key);
            }
            last_direction = direction;
            last_send_tick = xTaskGetTickCount();
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

    esp_lcd_touch_handle_t touch = nullptr;
    esp_err_t err = InitializeTouch(&touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch arrows disabled: %s", esp_err_to_name(err));
        return;
    }

    context.keyboard = &keyboard;
    context.touch = touch;

    BaseType_t task_ok = xTaskCreate(
        TouchArrowTask, "touch_arrows", 4096, &context, 5, nullptr);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch arrow task");
        return;
    }

    started = true;
    ESP_LOGI(TAG, "touch arrows started");
}
