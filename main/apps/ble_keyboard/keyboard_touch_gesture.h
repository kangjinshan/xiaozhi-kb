#ifndef KEYBOARD_TOUCH_GESTURE_H_
#define KEYBOARD_TOUCH_GESTURE_H_

#include <cstdint>

bool IsKeyboardSelectorHotCorner(uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height);

#endif  // KEYBOARD_TOUCH_GESTURE_H_
