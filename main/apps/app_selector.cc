#include "app_selector.h"

#include "app_mode.h"
#include "backlight.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdint>

#define TAG "app_selector"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

struct SelectorButtonContext {
    AppMode mode;
    KeyboardProfile profile;
    bool has_keyboard_profile;
    const char* log_name;
};

SelectorButtonContext kXiaozhiContext = {
    .mode = AppMode::kXiaozhi,
    .profile = KeyboardProfile::kProfile1,
    .has_keyboard_profile = false,
    .log_name = "xiaozhi",
};

SelectorButtonContext kRecorderContext = {
    .mode = AppMode::kRecorder,
    .profile = KeyboardProfile::kProfile1,  // 占位，不使用
    .has_keyboard_profile = false,
    .log_name = "recorder",
};

SelectorButtonContext kKeyboardProfile2Context = {
    .mode = AppMode::kKeyboard,
    .profile = KeyboardProfile::kProfile2,
    .has_keyboard_profile = true,
    .log_name = "keyboard_profile_2",
};

void OnSelectorButtonClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    auto* context = static_cast<SelectorButtonContext*>(lv_event_get_user_data(event));
    if (context == nullptr) {
        ESP_LOGW(TAG, "selector button missing context");
        return;
    }

    ESP_LOGI(TAG, "selected %s", context->log_name);
    if (context->has_keyboard_profile) {
        AppModeWriteKeyboardAndReboot(context->profile);
        return;
    }
    AppModeWriteAndReboot(context->mode);
}

void SetRoundedPanelStyle(lv_obj_t* obj, lv_color_t bg, lv_color_t border) {
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

lv_obj_t* CreateModeButton(lv_obj_t* parent,
                           const char* badge,
                           const char* label,
                           lv_color_t accent,
                           SelectorButtonContext* context) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 98);
    SetRoundedPanelStyle(button, lv_color_hex(0x1D222A), accent);
    lv_obj_set_style_pad_all(button, 14, 0);
    lv_obj_set_style_pad_column(button, 14, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(button, OnSelectorButtonClicked, LV_EVENT_CLICKED, context);

    lv_obj_t* badge_box = lv_obj_create(button);
    lv_obj_set_size(badge_box, 62, 62);
    lv_obj_set_style_radius(badge_box, 31, 0);
    lv_obj_set_style_bg_color(badge_box, accent, 0);
    lv_obj_set_style_bg_opa(badge_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge_box, 0, 0);
    lv_obj_clear_flag(badge_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* badge_label = lv_label_create(badge_box);
    lv_label_set_text(badge_label, badge);
    lv_obj_set_style_text_color(badge_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(badge_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_center(badge_label);

    lv_obj_t* text = lv_label_create(button);
    lv_label_set_text(text, label);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_style_text_color(text, lv_color_hex(0xF7F9FC), 0);
    lv_obj_set_style_text_font(text, &BUILTIN_TEXT_FONT, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);

    return button;
}

void BuildSelectorUI(Display* display) {
    DisplayLockGuard lock(display);

    const int width = std::max(display->width(), 320);
    const int height = std::max(display->height(), 320);

    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141A), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF7F9FC), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* root = lv_obj_create(screen);
    lv_obj_set_size(root, width, height);
    lv_obj_center(root);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_left(root, 28, 0);
    lv_obj_set_style_pad_right(root, 28, 0);
    lv_obj_set_style_pad_top(root, 34, 0);
    lv_obj_set_style_pad_bottom(root, 28, 0);
    lv_obj_set_style_pad_row(root, 12, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Select App");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);

    lv_obj_t* list = lv_obj_create(root);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    CreateModeButton(list,
                     "AI",
                     "XiaoZhi AI",
                     lv_color_hex(0x2E8BFF),
                     &kXiaozhiContext);
    CreateModeButton(list,
                     "REC",
                     "Recorder",
                     lv_color_hex(0x26A269),
                     &kRecorderContext);
    CreateModeButton(list,
                     "K2",
                     "Bluetooth KB",
                     lv_color_hex(0xD0A215),
                     &kKeyboardProfile2Context);
}

}  // namespace

void RunAppSelector() {
    ESP_LOGI(TAG, "boot -> app selector");

    auto& board = Board::GetInstance();
    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGE(TAG, "selector requires a display");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (auto* backlight = board.GetBacklight()) {
        backlight->RestoreBrightness();
    }

    BuildSelectorUI(display);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
