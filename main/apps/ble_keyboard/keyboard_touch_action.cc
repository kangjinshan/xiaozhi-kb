#include "keyboard_touch_action.h"

#include "keyboard_touch_gesture.h"
#include "touch_arrow_mapper.h"

namespace {

KeyboardTouchAction ArrowToAction(TouchArrowDirection direction) {
    switch (direction) {
        case TouchArrowDirection::kUp:
            return KeyboardTouchAction::kArrowUp;
        case TouchArrowDirection::kDown:
            return KeyboardTouchAction::kArrowDown;
        case TouchArrowDirection::kLeft:
            return KeyboardTouchAction::kArrowLeft;
        case TouchArrowDirection::kRight:
            return KeyboardTouchAction::kArrowRight;
        case TouchArrowDirection::kNone:
        default:
            return KeyboardTouchAction::kNone;
    }
}

KeyboardTouchAction MapProfile2Point(uint16_t x,
                                     uint16_t y,
                                     uint16_t width,
                                     uint16_t height) {
    const uint16_t zone_width = width / 3;
    const uint16_t zone_height = height / 3;
    if (zone_width == 0 || zone_height == 0) {
        return KeyboardTouchAction::kNone;
    }

    const bool left = x < zone_width;
    const bool right = x >= width - zone_width;
    const bool top = y < zone_height;
    const bool bottom = y >= height - zone_height;

    if (top && left) {
        return KeyboardTouchAction::kSelector;
    }
    if (bottom && left) {
        return KeyboardTouchAction::kBackspace;
    }
    if (top && right) {
        return KeyboardTouchAction::kEnter;
    }
    if (bottom && right) {
        return KeyboardTouchAction::kRightOption;
    }

    return ArrowToAction(MapTouchPointToArrow(x, y, width, height));
}

}  // namespace

KeyboardTouchAction MapTouchPointToKeyboardAction(KeyboardProfile profile,
                                                  uint16_t x,
                                                  uint16_t y,
                                                  uint16_t width,
                                                  uint16_t height) {
    if (width == 0 || height == 0) {
        return KeyboardTouchAction::kNone;
    }

    if (profile == KeyboardProfile::kProfile2) {
        return MapProfile2Point(x, y, width, height);
    }

    if (IsKeyboardSelectorHotCorner(x, y, width, height)) {
        return KeyboardTouchAction::kSelector;
    }

    return ArrowToAction(MapTouchPointToArrow(x, y, width, height));
}

const char* KeyboardTouchActionName(KeyboardTouchAction action) {
    switch (action) {
        case KeyboardTouchAction::kSelector:
            return "selector";
        case KeyboardTouchAction::kArrowUp:
            return "arrow_up";
        case KeyboardTouchAction::kArrowDown:
            return "arrow_down";
        case KeyboardTouchAction::kArrowLeft:
            return "arrow_left";
        case KeyboardTouchAction::kArrowRight:
            return "arrow_right";
        case KeyboardTouchAction::kBackspace:
            return "backspace";
        case KeyboardTouchAction::kEnter:
            return "enter";
        case KeyboardTouchAction::kRightOption:
            return "right_option";
        case KeyboardTouchAction::kNone:
        default:
            return "none";
    }
}
