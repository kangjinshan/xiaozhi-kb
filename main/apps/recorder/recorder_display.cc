#include "recorder_display.h"

#include "config.h"
#include "esp_lcd_sh8601.h"
#include "recorder_control_state.h"
#include "recorder_hold_input.h"
#include "recorder_display_guard.h"
#include "recorder_display_area.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <memory>
#include <string>
#include <vector>

#define TAG "recorder_display"

LV_FONT_DECLARE(font_puhui_shenyong_36_4);
LV_FONT_DECLARE(font_puhui_assistant_24_4);

namespace {

constexpr uint8_t kAxp2101LdoEnableReg = 0x90;
constexpr uint8_t kAxp2101Aldo3Mask = 0x04;

bool s_display_initialized = false;
bool s_screen_on = true;
int s_pause_depth = 0;
lv_display_t* s_display = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_obj_t* s_menu_button = nullptr;
lv_obj_t* s_brand_label = nullptr;
lv_obj_t* s_connection_dot = nullptr;
lv_obj_t* s_title_label = nullptr;
lv_obj_t* s_subtitle_label = nullptr;
lv_obj_t* s_metric_label = nullptr;
lv_obj_t* s_primary_button = nullptr;
lv_obj_t* s_primary_label = nullptr;
lv_obj_t* s_mic_icon = nullptr;
lv_obj_t* s_wave_icon = nullptr;
RecorderDisplayCallbacks s_callbacks;
RecorderAssistantPrimaryAction s_primary_action =
    RecorderAssistantPrimaryAction::kNone;
void* s_callback_user_data = nullptr;
uint32_t s_menu_pressed_tick = 0;
bool s_menu_hold_fired = false;
bool s_primary_holding = false;
RecorderDarkTouch s_dark_touch;
uint32_t s_dark_poll_ms = 0;
void OnInvalidateArea(lv_event_t* event) {
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
    if (area == nullptr) {
        return;
    }
    RecorderDisplayArea rounded = {
        .x1 = area->x1,
        .y1 = area->y1,
        .x2 = area->x2,
        .y2 = area->y2,
    };
    RecorderRoundDisplayArea(&rounded);
    area->x1 = rounded.x1;
    area->y1 = rounded.y1;
    area->x2 = rounded.x2;
    area->y2 = rounded.y2;
}

// SH8601 厂商初始化命令表（与键盘模式一致）
static const sh8601_lcd_init_cmd_t kVendorInit[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

esp_err_t WriteI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(device, buffer, sizeof(buffer), 100);
}

esp_err_t ReadI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(device, &reg, sizeof(reg), value, sizeof(*value), 100);
}

// AXP2101 ALDO3 断电-上电复位屏幕（与键盘模式一致）
esp_err_t ResetDisplayPower(i2c_master_dev_handle_t pmic) {
    uint8_t ldo_enable = 0;
    esp_err_t err = ReadI2cReg(pmic, kAxp2101LdoEnableReg, &ldo_enable);
    if (err != ESP_OK) return err;

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable | kAxp2101Aldo3Mask);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable & ~kAxp2101Aldo3Mask);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable | kAxp2101Aldo3Mask);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

// Latch the gesture, rather than inspecting the changing UI action on release.
// A release outside the button also ends recording; it can never leave the mic on.
void OnPrimaryEvent(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && !s_primary_holding &&
        s_primary_action == RecorderAssistantPrimaryAction::kTalk) {
        s_primary_holding = true;
        if (s_callbacks.record != nullptr) s_callbacks.record(s_callback_user_data);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_primary_holding) {
        s_primary_holding = false;
        if (s_callbacks.stop != nullptr) s_callbacks.stop(s_callback_user_data);
    }
}

void OnMenuEvent(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_menu_pressed_tick = lv_tick_get();
        s_menu_hold_fired = false;
    } else if (code == LV_EVENT_PRESSING && !s_menu_hold_fired &&
               RecorderMenuHoldReached(s_menu_pressed_tick, lv_tick_get())) {
        s_menu_hold_fired = true;
        if (s_callbacks.exit != nullptr) {
            s_callbacks.exit(s_callback_user_data);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_menu_hold_fired = false;
    }
}

void SetRect(lv_obj_t* object, const RecorderAssistantRect& rect) {
    lv_obj_set_pos(object, rect.x, rect.y);
    lv_obj_set_size(object, rect.width, rect.height);
}

lv_obj_t* CreateCenteredLabel(lv_obj_t* parent,
                              const RecorderAssistantRect& rect,
                              const char* text,
                              const lv_font_t* font,
                              lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    SetRect(label, rect);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

lv_obj_t* Shape(lv_obj_t* parent, int x, int y, int width, int height, int radius) {
    auto* object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x12241D), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

void CreateTalkIcons(lv_obj_t* button, int button_width) {
    const int center = button_width / 2;
    s_mic_icon = Shape(button, center - 52, 32, 104, 126, 0);
    lv_obj_set_style_bg_opa(s_mic_icon, LV_OPA_TRANSP, 0);
    auto* capsule = Shape(s_mic_icon, 32, 0, 40, 74, 20);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(capsule, lv_color_hex(0x12241D), 0);
    lv_obj_set_style_border_width(capsule, 5, 0);
    auto* cup = lv_arc_create(s_mic_icon);
    lv_obj_set_pos(cup, 16, 24);
    lv_obj_set_size(cup, 72, 72);
    lv_arc_set_bg_angles(cup, 0, 180);
    lv_obj_set_style_arc_width(cup, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(cup, lv_color_hex(0x12241D), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(cup, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(cup, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(cup, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(cup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cup, LV_OBJ_FLAG_SCROLLABLE);
    Shape(s_mic_icon, 49, 94, 6, 25, 3);
    Shape(s_mic_icon, 30, 117, 44, 5, 3);
    s_wave_icon = Shape(button, center - 52, 49, 104, 94, 0);
    lv_obj_set_style_bg_opa(s_wave_icon, LV_OPA_TRANSP, 0);
    Shape(s_wave_icon, 10, 26, 12, 42, 6);
    Shape(s_wave_icon, 46, 2, 12, 90, 6);
    Shape(s_wave_icon, 82, 26, 12, 42, 6);
    lv_obj_add_flag(s_wave_icon, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t InitTouch(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    touch_io_config.scl_speed_hz = 400 * 1000;
    esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_config, &s_touch_io);
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

    err = esp_lcd_touch_new_i2c_cst9217(s_touch_io, &touch_cfg, &s_touch);
    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_touch_cfg_t port_touch_cfg = {
        .disp = s_display,
        .handle = s_touch,
    };
    lvgl_port_add_touch(&port_touch_cfg);
    ESP_LOGI(TAG, "touch initialized");
    return ESP_OK;
}

}  // namespace

esp_err_t RecorderDisplayInit(i2c_master_bus_handle_t i2c_bus, i2c_master_dev_handle_t pmic) {
    if (s_display_initialized) {
        return ESP_OK;
    }
    if (i2c_bus == nullptr || pmic == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // SPI2 总线与 SD 卡共用；容忍已被初始化（幂等）。
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = LCD_PCLK;
    buscfg.data0_io_num = LCD_D0;
    buscfg.data1_io_num = LCD_D1;
    buscfg.data2_io_num = LCD_D2;
    buscfg.data3_io_num = LCD_D3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(LCD_CS, nullptr, nullptr);
    err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io);
    if (err != ESP_OK) return err;

    const sh8601_vendor_config_t vendor_config = {
        .init_cmds = &kVendorInit[0],
        .init_cmds_size = sizeof(kVendorInit) / sizeof(sh8601_lcd_init_cmd_t),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = (void*)&vendor_config;
    err = esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel);
    if (err != ESP_OK) return err;

    err = ResetDisplayPower(pmic);
    if (err != ESP_OK) return err;

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) return err;

    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) return err;

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(LCD_H_RES * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(LCD_H_RES),
        .vres = static_cast<uint32_t>(LCD_V_RES),
        .monochrome = false,
        .rotation = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    s_display = lvgl_port_add_disp(&display_cfg);
    if (s_display == nullptr) {
        return ESP_FAIL;
    }
    lv_display_set_default(s_display);
    lv_display_add_event_cb(
        s_display, OnInvalidateArea, LV_EVENT_INVALIDATE_AREA, nullptr);

    err = InitTouch(i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed: %s", esp_err_to_name(err));
    }

    // A static, single-action screen. No history tree or decorative AI orb.
    if (lvgl_port_lock(30000)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x10151C), 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(0xF1F6F4), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        const auto layout = RecorderBuildAssistantLayout(LCD_H_RES, LCD_V_RES);

        s_menu_button = lv_button_create(screen);
        SetRect(s_menu_button, layout.menu);
        lv_obj_set_style_bg_opa(s_menu_button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(s_menu_button, 0, 0);
        lv_obj_set_style_border_width(s_menu_button, 0, 0);
        lv_obj_add_event_cb(s_menu_button, OnMenuEvent, LV_EVENT_ALL, nullptr);
        auto* back = lv_label_create(s_menu_button);
        lv_label_set_text(back, "<");
        lv_obj_set_style_text_font(back, &font_puhui_assistant_24_4, 0);
        lv_obj_center(back);
        s_brand_label = CreateCenteredLabel(screen, layout.brand, "神勇",
            &font_puhui_shenyong_36_4, lv_color_hex(0xF1F6F4));
        s_connection_dot = Shape(screen, layout.connection.x, layout.connection.y,
                                layout.connection.width, layout.connection.height, LV_RADIUS_CIRCLE);
        s_title_label = CreateCenteredLabel(screen, layout.title, "",
            &font_puhui_assistant_24_4, lv_color_hex(0xBDC6C7));
        s_metric_label = CreateCenteredLabel(screen, layout.metric, "",
            &font_puhui_assistant_24_4, lv_color_hex(0xBDC6C7));
        s_subtitle_label = CreateCenteredLabel(screen, layout.subtitle, "",
            &font_puhui_assistant_24_4, lv_color_hex(0xBDC6C7));
        s_primary_button = lv_button_create(screen);
        SetRect(s_primary_button, layout.primary);
        lv_obj_set_style_pad_all(s_primary_button, 0, 0);
        lv_obj_set_style_radius(s_primary_button, 30, 0);
        lv_obj_set_style_border_width(s_primary_button, 0, 0);
        lv_obj_set_style_shadow_width(s_primary_button, 0, 0);
        lv_obj_remove_flag(s_primary_button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_primary_button, OnPrimaryEvent, LV_EVENT_ALL, nullptr);
        CreateTalkIcons(s_primary_button, layout.primary.width);
        s_primary_label = CreateCenteredLabel(s_primary_button,
            {0, layout.primary.height - 70, layout.primary.width, 48}, "",
            &font_puhui_shenyong_36_4, lv_color_hex(0x12241D));
        lvgl_port_unlock();
    }

    s_panel = panel;
    s_screen_on = true;
    s_display_initialized = true;
    return ESP_OK;
}

void RecorderDisplaySetCallbacks(const RecorderDisplayCallbacks& callbacks,
                                 void* user_data) {
    s_callbacks = callbacks;
    s_callback_user_data = user_data;
}

void RecorderDisplayRenderAssistant(const RecorderAssistantUiModel& model) {
    if (!s_display_initialized || s_primary_button == nullptr) return;
    if (!lvgl_port_lock(30000)) return;
    s_primary_action = model.primary_enabled ? model.primary_action
                                             : RecorderAssistantPrimaryAction::kNone;
    lv_obj_set_style_bg_color(s_connection_dot, lv_color_hex(model.connection_rgb), 0);
    lv_label_set_text(s_title_label, model.title.c_str());
    lv_label_set_text(s_subtitle_label, model.subtitle.c_str());
    lv_label_set_text(s_metric_label, model.metric.c_str());
    lv_label_set_text(s_primary_label, model.primary_label.c_str());
    const bool recording = model.primary_action == RecorderAssistantPrimaryAction::kSend;
    lv_obj_set_style_bg_color(s_primary_button,
        lv_color_hex(recording ? 0xFF9E9E : 0xBFF4D4), 0);
    lv_obj_set_style_bg_color(s_primary_button, lv_color_hex(0xFF9E9E), LV_STATE_PRESSED);
    lv_obj_set_style_opa(s_primary_button, model.primary_enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    if (recording) {
        lv_obj_add_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_wave_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wave_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (model.primary_enabled) lv_obj_remove_state(s_primary_button, LV_STATE_DISABLED);
    else lv_obj_add_state(s_primary_button, LV_STATE_DISABLED);
    lvgl_port_unlock();
}

void RecorderDisplayPause() {
    if (!s_display_initialized) {
        return;
    }
    if (s_pause_depth == 0) {
        esp_err_t err = lvgl_port_stop();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "failed to stop lvgl: %s", esp_err_to_name(err));
        }
        if (!lvgl_port_lock(30000)) {
            ESP_LOGW(TAG, "failed to lock lvgl for pause");
            lvgl_port_resume();
            return;
        }
    }
    ++s_pause_depth;
}

void RecorderDisplayResume() {
    if (!s_display_initialized || !RecorderDisplayCanResume(s_screen_on, s_pause_depth)) {
        return;
    }
    --s_pause_depth;
    if (s_pause_depth == 0) {
        lvgl_port_unlock();
        esp_err_t err = lvgl_port_resume();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "failed to resume lvgl: %s", esp_err_to_name(err));
        }
        lvgl_port_task_wake(LVGL_PORT_EVENT_USER, nullptr);
    }
}

esp_err_t RecorderDisplaySetScreenOn(bool screen_on) {
    if (!s_display_initialized || s_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (screen_on == s_screen_on) {
        return ESP_OK;
    }

    if (!screen_on) {
        const int previous_pause_depth = s_pause_depth;
        RecorderDisplayPause();
        if (s_pause_depth != previous_pause_depth + 1) {
            return ESP_ERR_TIMEOUT;
        }
        const esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
        if (err != ESP_OK) {
            RecorderDisplayResume();
            return err;
        }
        if (s_primary_holding) {
            s_primary_holding = false;
            s_dark_touch.AdoptHold();
        } else {
            s_dark_touch.Reset(true);
        }
        s_screen_on = false;
        ESP_LOGI(TAG, "PWR screen off");
        return ESP_OK;
    }

    if (s_pause_depth <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        return err;
    }
    if (s_dark_touch.Reset(true) == RecorderHoldAction::kStop) {
        // Continue the same touch in LVGL until its release; no extra turn.
        s_primary_holding = true;
    }
    s_screen_on = true;
    lv_obj_invalidate(lv_screen_active());
    RecorderDisplayResume();
    ESP_LOGI(TAG, "PWR screen on");
    return ESP_OK;
}

void RecorderDisplayPollDarkTouch() {
    if (!s_display_initialized || s_screen_on || s_touch == nullptr) return;
    // lvgl_port_stop also stops the LVGL tick timer; use the hardware clock.
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (static_cast<uint32_t>(now - s_dark_poll_ms) < 20) return;
    s_dark_poll_ms = now;
    // LVGL remains stopped, so only the main task reads the touch controller.
    // No display SPI transfer or panel wake occurs during this I2C poll.
    uint16_t x = 0, y = 0, strength = 0;
    uint8_t count = 0;
    const bool touched = esp_lcd_touch_read_data(s_touch) == ESP_OK &&
        esp_lcd_touch_get_coordinates(s_touch, &x, &y, &strength, &count, 1) && count > 0;
    const auto action = s_dark_touch.Poll(touched, now);
    if (action == RecorderHoldAction::kStart && s_callbacks.record) {
        ESP_LOGI(TAG, "dark touch recording start");
        s_callbacks.record(s_callback_user_data);
    } else if (action == RecorderHoldAction::kStop && s_callbacks.stop) {
        ESP_LOGI(TAG, "dark touch recording release");
        s_callbacks.stop(s_callback_user_data);
    }
}
