#ifndef TOUCH_ARROW_MAPPER_H_
#define TOUCH_ARROW_MAPPER_H_

#include <cstdint>

enum class TouchArrowDirection : uint8_t {
    kNone = 0,
    kUp,
    kDown,
    kLeft,
    kRight,
};

TouchArrowDirection MapTouchPointToArrow(uint16_t x,
                                         uint16_t y,
                                         uint16_t width,
                                         uint16_t height);

const char* TouchArrowDirectionName(TouchArrowDirection direction);

#endif  // TOUCH_ARROW_MAPPER_H_
