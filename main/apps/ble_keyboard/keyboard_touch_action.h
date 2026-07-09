#ifndef KEYBOARD_TOUCH_ACTION_H_
#define KEYBOARD_TOUCH_ACTION_H_

#include <cstdint>

enum class KeyboardProfile : uint8_t {
    kProfile1 = 1,
    kProfile2 = 2,
};

enum class KeyboardTouchAction : uint8_t {
    kNone = 0,
    kSelector,
    kArrowUp,
    kArrowDown,
    kArrowLeft,
    kArrowRight,
    kBackspace,
    kEnter,
    kRightOption,
};

KeyboardTouchAction MapTouchPointToKeyboardAction(KeyboardProfile profile,
                                                  uint16_t x,
                                                  uint16_t y,
                                                  uint16_t width,
                                                  uint16_t height);

const char* KeyboardTouchActionName(KeyboardTouchAction action);

#endif  // KEYBOARD_TOUCH_ACTION_H_
