#include "touchpad_motion.h"

#include <cstdio>
#include <cstdlib>

namespace {

void ExpectDelta(const char* name,
                 TouchpadDelta actual,
                 int expected_x,
                 int expected_y) {
    if (actual.x != expected_x || actual.y != expected_y) {
        std::fprintf(stderr,
                     "%s: expected (%d, %d), got (%d, %d)\n",
                     name,
                     expected_x,
                     expected_y,
                     actual.x,
                     actual.y);
        std::exit(1);
    }
}

}  // namespace

int main() {
    TouchpadMotion motion({
        .base_gain = 1.0f,
        .acceleration_start_pixels = 4,
        .acceleration_per_pixel = 0.1f,
        .maximum_gain = 2.0f,
        .maximum_sample_delta = 100,
    });

    ExpectDelta("first contact anchors", motion.Update(100, 100, true, true), 0, 0);
    ExpectDelta("right and down follow touch",
                motion.Update(103, 102, true, true),
                3,
                2);
    ExpectDelta("left and up follow touch",
                motion.Update(101, 99, true, true),
                -2,
                -3);
    ExpectDelta("fast drag accelerates",
                motion.Update(111, 99, true, true),
                16,
                0);

    ExpectDelta("lift resets tracking", motion.Update(0, 0, false, true), 0, 0);
    ExpectDelta("new contact after lift anchors",
                motion.Update(400, 400, true, true),
                0,
                0);

    ExpectDelta("disabled mode resets", motion.Update(405, 405, true, false), 0, 0);
    ExpectDelta("reenabled contact anchors",
                motion.Update(405, 405, true, true),
                0,
                0);

    motion.Reset();
    ExpectDelta("explicit reset anchors", motion.Update(10, 10, true, true), 0, 0);
    ExpectDelta("controller jump is rejected",
                motion.Update(300, 300, true, true),
                0,
                0);
    ExpectDelta("tracking resumes from rejected sample",
                motion.Update(301, 299, true, true),
                1,
                -1);

    TouchpadMotion clamped({
        .base_gain = 2.0f,
        .acceleration_start_pixels = 0,
        .acceleration_per_pixel = 0.0f,
        .maximum_gain = 2.0f,
        .maximum_sample_delta = 200,
    });
    ExpectDelta("clamp anchor", clamped.Update(100, 100, true, true), 0, 0);
    ExpectDelta("relative report clamps to HID range",
                clamped.Update(180, 20, true, true),
                127,
                -127);

    return 0;
}
