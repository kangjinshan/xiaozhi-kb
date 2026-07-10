#include "recorder_display.h"

#include "config.h"
#include "esp_lcd_sh8601.h"

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#define TAG "recorder_display"

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_30_4);

namespace {

constexpr uint8_t kAxp2101LdoEnableReg = 0x90;
constexpr uint8_t kAxp2101Aldo3Mask = 0x04;

bool s_display_initialized = false;
lv_display_t* s_display = nullptr;
lv_obj_t* s_title_label = nullptr;
lv_obj_t* s_subtitle_label = nullptr;

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

}  // namespace

esp_err_t RecorderDisplayInit(i2c_master_dev_handle_t pmic) {
    if (s_display_initialized) {
        return ESP_OK;
    }
    if (pmic == nullptr) {
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

    // 建好固定的两个标签，后续只更新文字，避免反复 clean/create。
    if (lvgl_port_lock(30000)) {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1115), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        s_title_label = lv_label_create(screen);
        lv_label_set_text(s_title_label, "");
        lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(s_title_label, &font_puhui_basic_30_4, 0);
        lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -20);

        s_subtitle_label = lv_label_create(screen);
        lv_label_set_text(s_subtitle_label, "");
        lv_obj_set_style_text_color(s_subtitle_label, lv_color_hex(0xA9B1BD), 0);
        lv_obj_set_style_text_font(s_subtitle_label, &font_puhui_basic_20_4, 0);
        lv_obj_align(s_subtitle_label, LV_ALIGN_CENTER, 0, 30);

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
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(s_subtitle_label, subtitle != nullptr ? subtitle : "");
    lv_obj_align(s_subtitle_label, LV_ALIGN_CENTER, 0, 30);
    lvgl_port_unlock();
}
