#include "keyboard_touch_gesture.h"

#include <cstdio>
#include <cstdlib>

static void ExpectHotCorner(const char* name, bool actual, bool expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     expected ? "true" : "false",
                     actual ? "true" : "false");
        std::exit(1);
    }
}

int main() {
    constexpr uint16_t kWidth = 480;
    constexpr uint16_t kHeight = 480;

    ExpectHotCorner("origin is hot corner",
                    IsKeyboardSelectorHotCorner(0, 0, kWidth, kHeight),
                    true);
    ExpectHotCorner("point inside one-fifth corner is hot",
                    IsKeyboardSelectorHotCorner(95, 95, kWidth, kHeight),
                    true);
    ExpectHotCorner("right boundary is outside",
                    IsKeyboardSelectorHotCorner(96, 10, kWidth, kHeight),
                    false);
    ExpectHotCorner("bottom boundary is outside",
                    IsKeyboardSelectorHotCorner(10, 96, kWidth, kHeight),
                    false);
    ExpectHotCorner("other screen area is not hot",
                    IsKeyboardSelectorHotCorner(240, 240, kWidth, kHeight),
                    false);
    ExpectHotCorner("zero width is invalid",
                    IsKeyboardSelectorHotCorner(0, 0, 0, kHeight),
                    false);
    ExpectHotCorner("zero height is invalid",
                    IsKeyboardSelectorHotCorner(0, 0, kWidth, 0),
                    false);

    return 0;
}
