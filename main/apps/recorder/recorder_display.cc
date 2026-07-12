#include "recorder_display.h"

#include "config.h"
#include "esp_lcd_sh8601.h"
#include "recorder_control_state.h"
#include "recorder_display_area.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <string>
#include <vector>

#define TAG "recorder_display"

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_assistant_24_4);

namespace {

constexpr uint8_t kAxp2101LdoEnableReg = 0x90;
constexpr uint8_t kAxp2101Aldo3Mask = 0x04;

bool s_display_initialized = false;
int s_pause_depth = 0;
lv_display_t* s_display = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_obj_t* s_menu_button = nullptr;
lv_obj_t* s_brand_label = nullptr;
lv_obj_t* s_connection_pill = nullptr;
lv_obj_t* s_connection_dot = nullptr;
lv_obj_t* s_connection_label = nullptr;
lv_obj_t* s_orb_outer = nullptr;
lv_obj_t* s_orb_inner = nullptr;
lv_obj_t* s_orb_label = nullptr;
lv_obj_t* s_title_label = nullptr;
lv_obj_t* s_subtitle_label = nullptr;
lv_obj_t* s_metric_label = nullptr;
lv_obj_t* s_primary_button = nullptr;
lv_obj_t* s_primary_label = nullptr;
lv_obj_t* s_history_button = nullptr;
lv_obj_t* s_file_menu = nullptr;
lv_obj_t* s_file_list = nullptr;
lv_obj_t* s_empty_label = nullptr;
RecorderDisplayCallbacks s_callbacks;
RecorderAssistantPrimaryAction s_primary_action =
    RecorderAssistantPrimaryAction::kNone;
void* s_callback_user_data = nullptr;
uint32_t s_menu_pressed_tick = 0;
bool s_menu_hold_fired = false;
std::vector<std::string> s_file_paths;

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

void OnHistoryClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_callbacks.open_menu == nullptr) {
        return;
    }
    s_callbacks.open_menu(s_callback_user_data);
}

void OnPrimaryClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    switch (s_primary_action) {
        case RecorderAssistantPrimaryAction::kTalk:
            if (s_callbacks.record != nullptr) {
                s_callbacks.record(s_callback_user_data);
            }
            break;
        case RecorderAssistantPrimaryAction::kSend:
            if (s_callbacks.stop != nullptr) {
                s_callbacks.stop(s_callback_user_data);
            }
            break;
        case RecorderAssistantPrimaryAction::kPause:
        case RecorderAssistantPrimaryAction::kResume:
            if (s_callbacks.pause_resume != nullptr) {
                s_callbacks.pause_resume(s_callback_user_data);
            }
            break;
        case RecorderAssistantPrimaryAction::kNone:
            break;
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

void OnFileClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_callbacks.play_file == nullptr) {
        return;
    }
    auto* path = static_cast<std::string*>(lv_event_get_user_data(event));
    if (path == nullptr) {
        return;
    }
    s_callbacks.play_file(path->c_str(), s_callback_user_data);
}

void OnBackClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_file_menu == nullptr) {
        return;
    }
    lv_obj_add_flag(s_file_menu, LV_OBJ_FLAG_HIDDEN);
}

void StyleButton(lv_obj_t* button, lv_color_t bg, lv_color_t border) {
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, bg, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, border, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_mix(bg, lv_color_hex(0xFFFFFF), 220),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
}

lv_obj_t* CreateButtonLabel(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_center(label);
    return label;
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

void SetFileMenuVisible(bool visible) {
    if (s_file_menu == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_file_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_file_menu);
    } else {
        lv_obj_add_flag(s_file_menu, LV_OBJ_FLAG_HIDDEN);
    }
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

    // 建好固定控件，后续只更新文字或列表，避免反复 clean/create。
    if (lvgl_port_lock(30000)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x090D16), 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(0xF7FAFF), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        const RecorderAssistantLayout layout =
            RecorderBuildAssistantLayout(LCD_H_RES, LCD_V_RES);

        s_menu_button = lv_button_create(screen);
        SetRect(s_menu_button, layout.menu);
        StyleButton(s_menu_button, lv_color_hex(0x151C29), lv_color_hex(0x39465A));
        lv_obj_add_event_cb(s_menu_button, OnMenuEvent, LV_EVENT_ALL, nullptr);
        CreateButtonLabel(s_menu_button, "MENU", &font_puhui_basic_20_4);

        s_brand_label = CreateCenteredLabel(
            screen, layout.brand, "金山 AI", &font_puhui_assistant_24_4,
            lv_color_hex(0xF7FAFF));

        s_connection_pill = lv_obj_create(screen);
        SetRect(s_connection_pill, layout.connection);
        lv_obj_set_style_radius(s_connection_pill, 18, 0);
        lv_obj_set_style_bg_color(s_connection_pill, lv_color_hex(0x111927), 0);
        lv_obj_set_style_bg_opa(s_connection_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_connection_pill, 1, 0);
        lv_obj_set_style_border_color(s_connection_pill, lv_color_hex(0x58D6FF), 0);
        lv_obj_set_style_pad_all(s_connection_pill, 0, 0);
        lv_obj_clear_flag(s_connection_pill, LV_OBJ_FLAG_SCROLLABLE);

        s_connection_dot = lv_obj_create(s_connection_pill);
        lv_obj_set_size(s_connection_dot, 10, 10);
        lv_obj_align(s_connection_dot, LV_ALIGN_LEFT_MID, 11, 0);
        lv_obj_set_style_radius(s_connection_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_connection_dot, lv_color_hex(0x58D6FF), 0);
        lv_obj_set_style_bg_opa(s_connection_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_connection_dot, 0, 0);
        lv_obj_clear_flag(s_connection_dot, LV_OBJ_FLAG_SCROLLABLE);

        s_connection_label = lv_label_create(s_connection_pill);
        lv_label_set_text(s_connection_label, "");
        lv_obj_set_width(s_connection_label, 88);
        lv_obj_align(s_connection_label, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_text_align(s_connection_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_connection_label, lv_color_hex(0xF7FAFF), 0);
        lv_obj_set_style_text_font(
            s_connection_label, &font_puhui_assistant_24_4, 0);

        s_orb_outer = lv_obj_create(screen);
        SetRect(s_orb_outer, layout.orb);
        lv_obj_set_style_radius(s_orb_outer, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_orb_outer, lv_color_hex(0x101C2B), 0);
        lv_obj_set_style_bg_opa(s_orb_outer, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_orb_outer, 3, 0);
        lv_obj_set_style_border_color(s_orb_outer, lv_color_hex(0x58D6FF), 0);
        lv_obj_set_style_pad_all(s_orb_outer, 0, 0);
        lv_obj_clear_flag(s_orb_outer, LV_OBJ_FLAG_SCROLLABLE);

        s_orb_inner = lv_obj_create(s_orb_outer);
        lv_obj_set_size(s_orb_inner, 116, 116);
        lv_obj_center(s_orb_inner);
        lv_obj_set_style_radius(s_orb_inner, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_orb_inner, lv_color_hex(0x58D6FF), 0);
        lv_obj_set_style_bg_opa(s_orb_inner, LV_OPA_30, 0);
        lv_obj_set_style_border_width(s_orb_inner, 0, 0);
        lv_obj_clear_flag(s_orb_inner, LV_OBJ_FLAG_SCROLLABLE);

        s_orb_label = lv_label_create(s_orb_inner);
        lv_label_set_text(s_orb_label, "AI");
        lv_obj_set_style_text_font(s_orb_label, &font_puhui_assistant_24_4, 0);
        lv_obj_set_style_text_color(s_orb_label, lv_color_hex(0xF7FAFF), 0);
        lv_obj_center(s_orb_label);

        s_title_label = CreateCenteredLabel(
            screen, layout.title, "", &font_puhui_assistant_24_4,
            lv_color_hex(0xF7FAFF));
        s_subtitle_label = CreateCenteredLabel(
            screen, layout.subtitle, "", &font_puhui_assistant_24_4,
            lv_color_hex(0x96A3B7));
        s_metric_label = CreateCenteredLabel(
            screen, layout.metric, "", &font_puhui_assistant_24_4,
            lv_color_hex(0x58D6FF));

        s_primary_button = lv_button_create(screen);
        SetRect(s_primary_button, layout.primary);
        StyleButton(s_primary_button, lv_color_hex(0x18364A), lv_color_hex(0x58D6FF));
        lv_obj_add_event_cb(
            s_primary_button, OnPrimaryClicked, LV_EVENT_CLICKED, nullptr);
        s_primary_label = CreateButtonLabel(
            s_primary_button, "", &font_puhui_assistant_24_4);

        s_history_button = lv_button_create(screen);
        SetRect(s_history_button, layout.history);
        StyleButton(s_history_button, lv_color_hex(0x151C29), lv_color_hex(0x39465A));
        lv_obj_add_event_cb(
            s_history_button, OnHistoryClicked, LV_EVENT_CLICKED, nullptr);
        CreateButtonLabel(s_history_button, "历史", &font_puhui_assistant_24_4);

        s_file_menu = lv_obj_create(screen);
        lv_obj_set_size(s_file_menu, LCD_H_RES, LCD_V_RES);
        lv_obj_center(s_file_menu);
        lv_obj_set_style_radius(s_file_menu, 0, 0);
        lv_obj_set_style_bg_color(s_file_menu, lv_color_hex(0x10141A), 0);
        lv_obj_set_style_bg_opa(s_file_menu, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_file_menu, 0, 0);
        lv_obj_set_style_pad_left(s_file_menu, 24, 0);
        lv_obj_set_style_pad_right(s_file_menu, 24, 0);
        lv_obj_set_style_pad_top(s_file_menu, 26, 0);
        lv_obj_set_style_pad_bottom(s_file_menu, 24, 0);
        lv_obj_set_style_pad_row(s_file_menu, 14, 0);
        lv_obj_set_flex_flow(s_file_menu, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(s_file_menu, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* header = lv_obj_create(s_file_menu);
        lv_obj_set_width(header, LV_PCT(100));
        lv_obj_set_height(header, 54);
        lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(header, 0, 0);
        lv_obj_set_style_pad_all(header, 0, 0);
        lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header,
                              LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* menu_spacer = lv_obj_create(header);
        lv_obj_set_size(menu_spacer, 100, 1);
        lv_obj_set_style_bg_opa(menu_spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(menu_spacer, 0, 0);
        lv_obj_set_style_pad_all(menu_spacer, 0, 0);
        lv_obj_clear_flag(menu_spacer, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* menu_title = lv_label_create(header);
        lv_label_set_text(menu_title, "对话历史");
        lv_obj_set_flex_grow(menu_title, 1);
        lv_obj_set_style_text_align(menu_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(menu_title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(menu_title, &font_puhui_assistant_24_4, 0);

        lv_obj_t* back_button = lv_button_create(header);
        lv_obj_set_size(back_button, 96, 46);
        StyleButton(back_button, lv_color_hex(0x2B313B), lv_color_hex(0x56606F));
        lv_obj_add_event_cb(back_button, OnBackClicked, LV_EVENT_CLICKED, nullptr);
        CreateButtonLabel(back_button, "BACK", &font_puhui_basic_20_4);

        s_file_list = lv_obj_create(s_file_menu);
        lv_obj_set_width(s_file_list, LV_PCT(100));
        lv_obj_set_flex_grow(s_file_list, 1);
        lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_file_list, 0, 0);
        lv_obj_set_style_pad_all(s_file_list, 0, 0);
        lv_obj_set_style_pad_row(s_file_list, 10, 0);
        lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(s_file_list, LV_DIR_VER);

        s_empty_label = lv_label_create(s_file_list);
        lv_label_set_text(s_empty_label, "暂无对话");
        lv_obj_set_width(s_empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_empty_label, &font_puhui_assistant_24_4, 0);

        lv_obj_add_flag(s_file_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_menu_button);

        lvgl_port_unlock();
    }

    s_display_initialized = true;
    return ESP_OK;
}

void RecorderDisplaySetCallbacks(const RecorderDisplayCallbacks& callbacks,
                                 void* user_data) {
    s_callbacks = callbacks;
    s_callback_user_data = user_data;
}

void RecorderDisplayRenderAssistant(const RecorderAssistantUiModel& model) {
    if (!s_display_initialized || s_connection_label == nullptr ||
        s_orb_outer == nullptr || s_orb_inner == nullptr ||
        s_title_label == nullptr || s_subtitle_label == nullptr ||
        s_metric_label == nullptr || s_primary_button == nullptr ||
        s_history_button == nullptr) {
        return;
    }
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }

    const lv_color_t accent = lv_color_hex(model.accent_rgb);
    const lv_color_t connection = lv_color_hex(model.connection_rgb);
    s_primary_action = model.primary_enabled
        ? model.primary_action
        : RecorderAssistantPrimaryAction::kNone;

    lv_label_set_text(s_connection_label, model.connection_label.c_str());
    lv_obj_set_style_bg_color(s_connection_dot, connection, 0);
    lv_obj_set_style_border_color(s_connection_pill, connection, 0);
    lv_obj_set_style_text_color(s_connection_label, connection, 0);

    lv_obj_set_style_border_color(s_orb_outer, accent, 0);
    lv_obj_set_style_bg_color(s_orb_outer,
                              lv_color_mix(lv_color_hex(0x090D16), accent, 205), 0);
    lv_obj_set_style_bg_color(s_orb_inner, accent, 0);
    lv_obj_set_style_text_color(s_orb_label, accent, 0);

    lv_label_set_text(s_title_label, model.title.c_str());
    lv_label_set_text(s_subtitle_label, model.subtitle.c_str());
    lv_label_set_text(s_metric_label, model.metric.c_str());
    lv_obj_set_style_text_color(s_metric_label, accent, 0);

    lv_label_set_text(s_primary_label, model.primary_label.c_str());
    StyleButton(s_primary_button,
                lv_color_mix(lv_color_hex(0x101827), accent, 185), accent);
    lv_obj_set_style_opa(s_primary_button,
                         model.primary_enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    if (model.primary_enabled) {
        lv_obj_remove_state(s_primary_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_primary_button, LV_STATE_DISABLED);
    }

    if (model.history_visible) {
        lv_obj_remove_flag(s_history_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_history_button, LV_OBJ_FLAG_HIDDEN);
    }
    SetFileMenuVisible(false);
    lv_obj_move_foreground(s_menu_button);
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
    if (!s_display_initialized || s_pause_depth <= 0) {
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

void RecorderDisplayShowFileMenu(const std::vector<RecorderDisplayMenuItem>& items) {
    if (!s_display_initialized || s_file_list == nullptr) {
        return;
    }
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }

    lv_obj_clean(s_file_list);
    s_file_paths.clear();
    s_file_paths.reserve(items.size());
    for (const auto& item : items) {
        s_file_paths.push_back(item.path);
    }

    if (items.empty()) {
        s_empty_label = lv_label_create(s_file_list);
        lv_label_set_text(s_empty_label, "暂无对话");
        lv_obj_set_width(s_empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_empty_label, &font_puhui_assistant_24_4, 0);
    } else {
        for (size_t i = 0; i < items.size(); ++i) {
            lv_obj_t* row = lv_button_create(s_file_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 82);
            StyleButton(row, lv_color_hex(0x151C29), lv_color_hex(0x39465A));
            lv_obj_set_style_pad_left(row, 16, 0);
            lv_obj_set_style_pad_right(row, 16, 0);
            lv_obj_set_style_pad_top(row, 8, 0);
            lv_obj_set_style_pad_bottom(row, 8, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(row,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
            lv_obj_add_event_cb(row, OnFileClicked, LV_EVENT_CLICKED, &s_file_paths[i]);

            lv_obj_t* name = lv_label_create(row);
            lv_label_set_text(name, items[i].label.c_str());
            lv_obj_set_width(name, LV_PCT(100));
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(name, &font_puhui_assistant_24_4, 0);

            lv_obj_t* detail = lv_label_create(row);
            lv_label_set_text(detail, items[i].detail.c_str());
            lv_obj_set_width(detail, LV_PCT(100));
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(detail, lv_color_hex(0xA9B1BD), 0);
            lv_obj_set_style_text_font(detail, &font_puhui_basic_20_4, 0);
        }
    }

    SetFileMenuVisible(true);
    lv_obj_move_foreground(s_menu_button);
    lvgl_port_unlock();
}

void RecorderDisplayHideFileMenu() {
    if (!s_display_initialized || s_file_menu == nullptr) {
        return;
    }
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }
    SetFileMenuVisible(false);
    lvgl_port_unlock();
}
