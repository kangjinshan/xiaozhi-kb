#include "touch_arrow_mapper.h"

#include <cstdlib>

TouchArrowDirection MapTouchPointToArrow(uint16_t x,
                                         uint16_t y,
                                         uint16_t width,
                                         uint16_t height) {
    if (width == 0 || height == 0) {
        return TouchArrowDirection::kNone;
    }

    const int32_t center_x = static_cast<int32_t>(width) / 2;
    const int32_t center_y = static_cast<int32_t>(height) / 2;
    const int32_t dx = static_cast<int32_t>(x) - center_x;
    const int32_t dy = static_cast<int32_t>(y) - center_y;

    if (std::abs(dx) > std::abs(dy)) {
        return dx < 0 ? TouchArrowDirection::kLeft : TouchArrowDirection::kRight;
    }

    return dy < 0 ? TouchArrowDirection::kUp : TouchArrowDirection::kDown;
}

const char* TouchArrowDirectionName(TouchArrowDirection direction) {
    switch (direction) {
        case TouchArrowDirection::kUp:
            return "up";
        case TouchArrowDirection::kDown:
            return "down";
        case TouchArrowDirection::kLeft:
            return "left";
        case TouchArrowDirection::kRight:
            return "right";
        case TouchArrowDirection::kNone:
        default:
            return "none";
    }
}
