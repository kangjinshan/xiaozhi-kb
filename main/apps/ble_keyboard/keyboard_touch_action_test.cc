#include "keyboard_touch_action.h"

#include <cstdio>
#include <cstdlib>

static void ExpectAction(const char* name,
                         KeyboardTouchAction actual,
                         KeyboardTouchAction expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     KeyboardTouchActionName(expected),
                     KeyboardTouchActionName(actual));
        std::exit(1);
    }
}

int main() {
    constexpr uint16_t kWidth = 480;
    constexpr uint16_t kHeight = 480;

    ExpectAction("profile1 top left remains selector",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile1, 10, 10, kWidth, kHeight),
                 KeyboardTouchAction::kSelector);
    ExpectAction("profile1 bottom left remains arrow left",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile1, 10, 300, kWidth, kHeight),
                 KeyboardTouchAction::kArrowLeft);
    ExpectAction("profile1 right side remains arrow right",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile1, 470, 240, kWidth, kHeight),
                 KeyboardTouchAction::kArrowRight);

    ExpectAction("profile2 top left is selector",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 10, 10, kWidth, kHeight),
                 KeyboardTouchAction::kSelector);
    ExpectAction("profile2 bottom left is backspace",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 10, 470, kWidth, kHeight),
                 KeyboardTouchAction::kBackspace);
    ExpectAction("profile2 top right is enter",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 470, 10, kWidth, kHeight),
                 KeyboardTouchAction::kEnter);
    ExpectAction("profile2 bottom right is right option",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 470, 470, kWidth, kHeight),
                 KeyboardTouchAction::kRightOption);
    ExpectAction("profile2 top center is arrow up",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 240, 10, kWidth, kHeight),
                 KeyboardTouchAction::kArrowUp);
    ExpectAction("profile2 bottom center is arrow down",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 240, 470, kWidth, kHeight),
                 KeyboardTouchAction::kArrowDown);
    ExpectAction("profile2 left center is arrow left",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 10, 240, kWidth, kHeight),
                 KeyboardTouchAction::kArrowLeft);
    ExpectAction("profile2 right center is arrow right",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 470, 240, kWidth, kHeight),
                 KeyboardTouchAction::kArrowRight);
    ExpectAction("profile2 center is left command",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 240, 240, kWidth, kHeight),
                 KeyboardTouchAction::kLeftCommand);
    ExpectAction("zero width is invalid",
                 MapTouchPointToKeyboardAction(
                     KeyboardProfile::kProfile2, 10, 10, 0, kHeight),
                 KeyboardTouchAction::kNone);

    return 0;
}
