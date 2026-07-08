#include "keyboard_touch_gesture.h"

bool IsKeyboardSelectorHotCorner(uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height) {
    if (width == 0 || height == 0) {
        return false;
    }

    const uint16_t hot_width = width / 5;
    const uint16_t hot_height = height / 5;
    if (hot_width == 0 || hot_height == 0) {
        return false;
    }

    return x < hot_width && y < hot_height;
}
