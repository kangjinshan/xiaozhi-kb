#include "recorder_display_area.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void TestOddStartAndEvenEndExpandOutward() {
    RecorderDisplayArea area{135, 279, 344, 340};
    RecorderRoundDisplayArea(&area);
    Check(area.x1 == 134, "odd x start rounds down");
    Check(area.y1 == 278, "odd y start rounds down");
    Check(area.x2 == 345, "even x end rounds up");
    Check(area.y2 == 341, "even y end rounds up");
}

void TestAlignedAreaStaysUnchanged() {
    RecorderDisplayArea area{134, 278, 345, 341};
    RecorderRoundDisplayArea(&area);
    Check(area.x1 == 134 && area.y1 == 278 &&
              area.x2 == 345 && area.y2 == 341,
          "already aligned area stays unchanged");
}

}  // namespace

int main() {
    TestOddStartAndEvenEndExpandOutward();
    TestAlignedAreaStaysUnchanged();
    std::puts("recorder_display_area_test passed");
    return 0;
}
