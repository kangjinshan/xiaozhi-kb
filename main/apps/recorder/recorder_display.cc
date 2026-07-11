#include "recorder_display.h"

#include "config.h"
#include "esp_lcd_sh8601.h"

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
LV_FONT_DECLARE(font_puhui_basic_30_4);

namespace {

constexpr uint8_t kAxp2101LdoEnableReg = 0x90;
constexpr uint8_t kAxp2101Aldo3Mask = 0x04;

bool s_display_initialized = false;
int s_pause_depth = 0;
lv_display_t* s_display = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;
lv_obj_t* s_title_label = nullptr;
lv_obj_t* s_subtitle_label = nullptr;
lv_obj_t* s_play_button = nullptr;
lv_obj_t* s_file_menu = nullptr;
lv_obj_t* s_file_list = nullptr;
lv_obj_t* s_empty_label = nullptr;
RecorderDisplayCallback s_open_menu_callback = nullptr;
RecorderDisplayFileCallback s_play_file_callback = nullptr;
void* s_callback_user_data = nullptr;
std::vector<std::string> s_file_paths;

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

void OnPlayMenuClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_open_menu_callback == nullptr) {
        return;
    }
    s_open_menu_callback(s_callback_user_data);
}

void OnFileClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_play_file_callback == nullptr) {
        return;
    }
    auto* path = static_cast<std::string*>(lv_event_get_user_data(event));
    if (path == nullptr) {
        return;
    }
    s_play_file_callback(path->c_str(), s_callback_user_data);
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
}

lv_obj_t* CreateButtonLabel(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_center(label);
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

    err = InitTouch(i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed: %s", esp_err_to_name(err));
    }

    // 建好固定控件，后续只更新文字或列表，避免反复 clean/create。
    if (lvgl_port_lock(30000)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(0xF7F9FC), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        s_title_label = lv_label_create(screen);
        lv_label_set_text(s_title_label, "");
        lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_title_label, &font_puhui_basic_30_4, 0);
        lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -60);

        s_subtitle_label = lv_label_create(screen);
        lv_label_set_text(s_subtitle_label, "");
        lv_obj_set_style_text_color(s_subtitle_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_subtitle_label, &font_puhui_basic_20_4, 0);
        lv_obj_align(s_subtitle_label, LV_ALIGN_CENTER, 0, -12);

        s_play_button = lv_button_create(screen);
        lv_obj_set_size(s_play_button, 180, 58);
        lv_obj_align(s_play_button, LV_ALIGN_CENTER, 0, 66);
        StyleButton(s_play_button, lv_color_hex(0x238A54), lv_color_hex(0x69DB9C));
        lv_obj_add_event_cb(s_play_button, OnPlayMenuClicked, LV_EVENT_CLICKED, nullptr);
        CreateButtonLabel(s_play_button, "PLAY", &font_puhui_basic_20_4);
        lv_obj_add_flag(s_play_button, LV_OBJ_FLAG_HIDDEN);

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

        lv_obj_t* menu_title = lv_label_create(header);
        lv_label_set_text(menu_title, "Recordings");
        lv_obj_set_style_text_color(menu_title, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(menu_title, &font_puhui_basic_30_4, 0);

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
        lv_label_set_text(s_empty_label, "No recordings");
        lv_obj_set_width(s_empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_empty_label, &font_puhui_basic_20_4, 0);

        lv_obj_add_flag(s_file_menu, LV_OBJ_FLAG_HIDDEN);

        lvgl_port_unlock();
    }

    s_display_initialized = true;
    return ESP_OK;
}

void RecorderShowText(const char* title, const char* subtitle) {
    if (!s_display_initialized || s_title_label == nullptr) {
        return;
    }
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }
    lv_label_set_text(s_title_label, title != nullptr ? title : "");
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -60);
    lv_label_set_text(s_subtitle_label, subtitle != nullptr ? subtitle : "");
    lv_obj_align(s_subtitle_label, LV_ALIGN_CENTER, 0, -12);
    lvgl_port_unlock();
}

void RecorderDisplaySetCallbacks(RecorderDisplayCallback open_menu,
                                 RecorderDisplayFileCallback play_file,
                                 void* user_data) {
    s_open_menu_callback = open_menu;
    s_play_file_callback = play_file;
    s_callback_user_data = user_data;
}

void RecorderDisplaySetPlayMenuVisible(bool visible) {
    if (!s_display_initialized || s_play_button == nullptr) {
        return;
    }
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_play_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_play_button, LV_OBJ_FLAG_HIDDEN);
    }
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
        lv_label_set_text(s_empty_label, "No recordings");
        lv_obj_set_width(s_empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_empty_label, &font_puhui_basic_20_4, 0);
    } else {
        for (size_t i = 0; i < items.size(); ++i) {
            lv_obj_t* row = lv_button_create(s_file_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 74);
            StyleButton(row, lv_color_hex(0x1D222A), lv_color_hex(0x3E4754));
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
            lv_obj_set_style_text_font(name, &font_puhui_basic_20_4, 0);

            lv_obj_t* detail = lv_label_create(row);
            lv_label_set_text(detail, items[i].detail.c_str());
            lv_obj_set_width(detail, LV_PCT(100));
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(detail, lv_color_hex(0xA9B1BD), 0);
            lv_obj_set_style_text_font(detail, &font_puhui_basic_20_4, 0);
        }
    }

    SetFileMenuVisible(true);
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
