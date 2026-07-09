#include "keyboard_zone_display.h"

#include "config.h"
#include "esp_lcd_sh8601.h"
#include "keyboard_zone_display_state.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#define TAG "kb_zone_display"

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_30_4);

namespace {

constexpr uint8_t kAxp2101LdoEnableReg = 0x90;
constexpr uint8_t kAxp2101Aldo3Mask = 0x04;

bool s_display_initialized = false;
bool s_guide_visible = false;
lv_display_t* s_display = nullptr;

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

esp_err_t ResetDisplayPower(i2c_master_dev_handle_t pmic) {
    uint8_t ldo_enable = 0;
    esp_err_t err = ReadI2cReg(pmic, kAxp2101LdoEnableReg, &ldo_enable);
    if (err != ESP_OK) {
        return err;
    }

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable | kAxp2101Aldo3Mask);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable & ~kAxp2101Aldo3Mask);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    err = WriteI2cReg(pmic, kAxp2101LdoEnableReg, ldo_enable | kAxp2101Aldo3Mask);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t InitializeDisplay(i2c_master_dev_handle_t pmic) {
    if (s_display_initialized) {
        return ESP_OK;
    }

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
    if (err != ESP_OK) {
        return err;
    }

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
    if (err != ESP_OK) {
        return err;
    }

    err = ResetDisplayPower(pmic);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }

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

    s_display_initialized = true;
    return ESP_OK;
}

void StyleCell(lv_obj_t* cell, lv_color_t bg, lv_color_t border) {
    lv_obj_set_style_radius(cell, 4, 0);
    lv_obj_set_style_bg_color(cell, bg, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_border_color(cell, border, 0);
    lv_obj_set_style_pad_all(cell, 4, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
}

void AddCell(lv_obj_t* parent,
             const char* text,
             lv_color_t bg,
             lv_color_t border,
             int col,
             int row) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_set_grid_cell(cell,
                         LV_GRID_ALIGN_STRETCH,
                         col,
                         1,
                         LV_GRID_ALIGN_STRETCH,
                         row,
                         1);
    StyleCell(cell, bg, border);

    lv_obj_t* label = lv_label_create(cell);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, &font_puhui_basic_30_4, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
}

void DrawZoneGuide() {
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "PROFILE 2 ZONES");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &font_puhui_basic_20_4, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t* grid = lv_obj_create(screen);
    lv_obj_set_size(grid, LCD_H_RES - 36, LCD_V_RES - 86);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    static lv_coord_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, cols, rows);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    AddCell(grid, "MENU", lv_color_hex(0x22577A), lv_color_hex(0x57CC99), 0, 0);
    AddCell(grid, "UP", lv_color_hex(0x1D3557), lv_color_hex(0x86B7FE), 1, 0);
    AddCell(grid, "ENTER", lv_color_hex(0x7A4E00), lv_color_hex(0xFFD166), 2, 0);
    AddCell(grid, "LEFT", lv_color_hex(0x1D3557), lv_color_hex(0x86B7FE), 0, 1);
    AddCell(grid, "", lv_color_hex(0x222831), lv_color_hex(0x565F6B), 1, 1);
    AddCell(grid, "RIGHT", lv_color_hex(0x1D3557), lv_color_hex(0x86B7FE), 2, 1);
    AddCell(grid, "BKSP", lv_color_hex(0x6A1B1A), lv_color_hex(0xFF8A80), 0, 2);
    AddCell(grid, "DOWN", lv_color_hex(0x1D3557), lv_color_hex(0x86B7FE), 1, 2);
    AddCell(grid, "OPT", lv_color_hex(0x4B3869), lv_color_hex(0xC7A4FF), 2, 2);

    lvgl_port_unlock();
}

void DrawBlackScreen() {
    if (!lvgl_port_lock(30000)) {
        ESP_LOGW(TAG, "failed to lock lvgl");
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lvgl_port_unlock();
}

}  // namespace

esp_err_t KeyboardZoneDisplayToggle(i2c_master_dev_handle_t pmic) {
    if (pmic == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = InitializeDisplay(pmic);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zone display init failed: %s", esp_err_to_name(err));
        return err;
    }

    const bool next_visible = NextKeyboardZoneGuideVisible(s_guide_visible);
    if (next_visible) {
        DrawZoneGuide();
    } else {
        DrawBlackScreen();
    }
    s_guide_visible = next_visible;
    return ESP_OK;
}
