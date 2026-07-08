#include "touch_arrow_mapper.h"

#include <cstdio>
#include <cstdlib>

static void ExpectDirection(const char* name,
                            TouchArrowDirection actual,
                            TouchArrowDirection expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     TouchArrowDirectionName(expected),
                     TouchArrowDirectionName(actual));
        std::exit(1);
    }
}

int main() {
    constexpr uint16_t kWidth = 480;
    constexpr uint16_t kHeight = 480;

    ExpectDirection("top center maps to up",
                    MapTouchPointToArrow(240, 10, kWidth, kHeight),
                    TouchArrowDirection::kUp);
    ExpectDirection("bottom center maps to down",
                    MapTouchPointToArrow(240, 470, kWidth, kHeight),
                    TouchArrowDirection::kDown);
    ExpectDirection("left center maps to left",
                    MapTouchPointToArrow(10, 240, kWidth, kHeight),
                    TouchArrowDirection::kLeft);
    ExpectDirection("right center maps to right",
                    MapTouchPointToArrow(470, 240, kWidth, kHeight),
                    TouchArrowDirection::kRight);
    ExpectDirection("dominant horizontal offset wins",
                    MapTouchPointToArrow(20, 200, kWidth, kHeight),
                    TouchArrowDirection::kLeft);
    ExpectDirection("dominant vertical offset wins",
                    MapTouchPointToArrow(220, 20, kWidth, kHeight),
                    TouchArrowDirection::kUp);
    ExpectDirection("center falls back to down",
                    MapTouchPointToArrow(240, 240, kWidth, kHeight),
                    TouchArrowDirection::kDown);
    ExpectDirection("diagonal tie falls back to vertical direction",
                    MapTouchPointToArrow(260, 260, kWidth, kHeight),
                    TouchArrowDirection::kDown);
    ExpectDirection("zero width is invalid",
                    MapTouchPointToArrow(10, 10, 0, kHeight),
                    TouchArrowDirection::kNone);
    ExpectDirection("zero height is invalid",
                    MapTouchPointToArrow(10, 10, kWidth, 0),
                    TouchArrowDirection::kNone);

    return 0;
}
