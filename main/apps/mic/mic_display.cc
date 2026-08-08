#include "mic_display.h"

#include "config.h"

#include <cstdio>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "mic_display"

LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

MicDisplayCallbacks s_callbacks;
void* s_callback_user_data = nullptr;
lv_obj_t* s_connection = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_time = nullptr;
lv_obj_t* s_gain = nullptr;
lv_obj_t* s_record_button = nullptr;
lv_obj_t* s_record_label = nullptr;
lv_obj_t* s_menu_button = nullptr;

void StyleButton(lv_obj_t* button, lv_color_t background, lv_color_t border) {
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, background, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, border, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
}

lv_obj_t* Label(lv_obj_t* parent,
                const char* text,
                const lv_font_t* font,
                lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

void OnRecord(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.toggle_recording != nullptr) {
        s_callbacks.toggle_recording(s_callback_user_data);
    }
}

void OnMenu(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.exit != nullptr) {
        s_callbacks.exit(s_callback_user_data);
    }
}

}  // namespace

bool MicDisplayInit(const MicDisplayCallbacks& callbacks, void* user_data) {
    s_callbacks = callbacks;
    s_callback_user_data = user_data;
    if (!lvgl_port_lock(30000)) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return false;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF7FAFF), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_menu_button = lv_button_create(screen);
    lv_obj_set_pos(s_menu_button, 24, 22);
    lv_obj_set_size(s_menu_button, 100, 52);
    StyleButton(s_menu_button, lv_color_hex(0x151C29), lv_color_hex(0x58647A));
    lv_obj_add_event_cb(s_menu_button, OnMenu, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* menu_label = Label(s_menu_button, "MENU", &font_puhui_basic_20_4,
                                 lv_color_hex(0xFFFFFF));
    lv_obj_center(menu_label);

    s_connection = Label(screen, "WIFI WAIT", &font_puhui_basic_20_4,
                         lv_color_hex(0xA7B0C0));
    lv_obj_set_pos(s_connection, 298, 34);
    lv_obj_set_size(s_connection, 150, 34);

    s_title = Label(screen, "Stereo Wi-Fi Mic", &BUILTIN_TEXT_FONT,
                    lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_title, 34, 98);
    lv_obj_set_size(s_title, 412, 48);

    lv_obj_t* channel = Label(screen, "L  MIC1     MIC2  R",
                              &font_puhui_basic_20_4,
                              lv_color_hex(0xC084FC));
    lv_obj_set_pos(channel, 70, 160);
    lv_obj_set_size(channel, 340, 36);

    s_status = Label(screen, "READY - ESP-SR NS", &font_puhui_basic_20_4,
                     lv_color_hex(0xA7B0C0));
    lv_obj_set_pos(s_status, 42, 210);
    lv_obj_set_size(s_status, 396, 38);

    s_time = Label(screen, "00:00", &BUILTIN_TEXT_FONT,
                   lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_time, 130, 262);
    lv_obj_set_size(s_time, 220, 58);

    s_gain = Label(screen, "GAIN 70%   KEY+ / BOOT-",
                   &font_puhui_basic_20_4,
                   lv_color_hex(0xC084FC));
    lv_obj_set_pos(s_gain, 50, 326);
    lv_obj_set_size(s_gain, 380, 36);

    s_record_button = lv_button_create(screen);
    lv_obj_set_pos(s_record_button, 92, 380);
    lv_obj_set_size(s_record_button, 296, 72);
    StyleButton(s_record_button, lv_color_hex(0x4A173D),
                lv_color_hex(0xF43F5E));
    lv_obj_add_event_cb(s_record_button, OnRecord, LV_EVENT_CLICKED, nullptr);
    s_record_label = Label(s_record_button, "START RECORDING",
                           &font_puhui_basic_20_4,
                           lv_color_hex(0xFFFFFF));
    lv_obj_center(s_record_label);

    lvgl_port_unlock();
    return true;
}

void MicDisplayRender(const MicDisplayModel& model) {
    if (s_connection == nullptr || !lvgl_port_lock(30000)) {
        return;
    }

    const lv_color_t purple = lv_color_hex(0xC084FC);
    const lv_color_t green = lv_color_hex(0x34D399);
    const lv_color_t red = lv_color_hex(0xF43F5E);
    lv_label_set_text(s_connection,
                      model.wifi_streaming ? "WIFI STREAM" :
                      model.wifi_connected ? "WIFI READY" : "WIFI WAIT");
    lv_obj_set_style_text_color(
        s_connection, model.wifi_connected ? green : lv_color_hex(0xA7B0C0), 0);

    if (model.recording) {
        lv_label_set_text(s_status,
                          model.wifi_streaming ? "RECORDING + LIVE" : "RECORDING TO SD");
        lv_obj_set_style_text_color(s_status, red, 0);
        lv_label_set_text(s_record_label, "STOP RECORDING");
        StyleButton(s_record_button, lv_color_hex(0x5A1728), red);
    } else if (!model.sd_ready) {
        lv_label_set_text(s_status, "READY - SD UNAVAILABLE");
        lv_obj_set_style_text_color(s_status, red, 0);
        lv_label_set_text(s_record_label, "SD CARD REQUIRED");
        StyleButton(s_record_button, lv_color_hex(0x2B303A),
                    lv_color_hex(0x58647A));
    } else {
        lv_label_set_text(s_status,
                          model.noise_reduction ? "READY - ESP-SR NS" : "READY - NS OFF");
        lv_obj_set_style_text_color(s_status,
                                    model.noise_reduction ? green : red, 0);
        lv_label_set_text(s_record_label, "START RECORDING");
        StyleButton(s_record_button, lv_color_hex(0x2D1648), purple);
    }
    if (model.sd_ready) {
        lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_record_button, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_record_button, LV_OPA_40, 0);
    }

    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
                  static_cast<unsigned long>(model.elapsed_seconds / 60),
                  static_cast<unsigned long>(model.elapsed_seconds % 60));
    lv_label_set_text(s_time, buffer);
    std::snprintf(buffer, sizeof(buffer), "GAIN %d%%   KEY+ / BOOT-",
                  model.gain_percent);
    lv_label_set_text(s_gain, buffer);

    lvgl_port_unlock();
}
