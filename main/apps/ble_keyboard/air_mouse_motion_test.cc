#include "air_mouse_motion.h"

#include <cstdio>
#include <cstdlib>

namespace {

void ExpectDelta(const char* name,
                 AirMouseDelta actual,
                 int expected_x,
                 int expected_y) {
    if (actual.x != expected_x || actual.y != expected_y) {
        std::fprintf(stderr,
                     "%s: expected (%d,%d), got (%d,%d)\n",
                     name,
                     expected_x,
                     expected_y,
                     static_cast<int>(actual.x),
                     static_cast<int>(actual.y));
        std::exit(1);
    }
}

AirMouseMotionConfig DeterministicConfig() {
    AirMouseMotionConfig config;
    config.min_cutoff_hz = 1000.0f;
    config.beta = 0.0f;
    config.derivative_cutoff_hz = 1000.0f;
    config.deadzone_dps = 0.0f;
    config.pixels_per_degree = 11.0f;
    config.acceleration_start_dps = 10000.0f;
    config.acceleration_per_dps = 0.0f;
    config.maximum_gain = 1.0f;
    return config;
}

}  // namespace

int main() {
    {
        AirMouseMotion motion(DeterministicConfig());
        ExpectDelta("disabled motion",
                    motion.Update(100.0f, 100.0f, 10000, false),
                    0,
                    0);
        ExpectDelta("first active sample primes filter",
                    motion.Update(10.0f, -20.0f, 20000, true),
                    0,
                    0);
        ExpectDelta("constant rate becomes relative pixels",
                    motion.Update(10.0f, -20.0f, 30000, true),
                    1,
                    -2);
    }

    {
        AirMouseMotionConfig config = DeterministicConfig();
        config.deadzone_dps = 1.0f;
        AirMouseMotion motion(config);
        motion.Update(0.5f, -0.8f, 10000, true);
        ExpectDelta("deadzone suppresses stationary noise",
                    motion.Update(0.5f, -0.8f, 20000, true),
                    0,
                    0);
    }

    {
        AirMouseMotion motion(DeterministicConfig());
        motion.Update(0.4f, 0.0f, 10000, true);
        int emitted_x = 0;
        for (uint64_t timestamp = 20000; timestamp <= 270000;
             timestamp += 10000) {
            emitted_x += motion.Update(0.4f, 0.0f, timestamp, true).x;
        }
        if (emitted_x != 1) {
            std::fprintf(stderr,
                         "fractional pixels: expected total 1, got %d\n",
                         emitted_x);
            return 1;
        }
    }

    {
        AirMouseMotion motion(DeterministicConfig());
        motion.Update(5000.0f, -5000.0f, 10000, true);
        ExpectDelta("relative report clamps to HID range",
                    motion.Update(5000.0f, -5000.0f, 20000, true),
                    127,
                    -127);
    }

    {
        AirMouseMotion motion(DeterministicConfig());
        motion.Update(9.0f, 0.0f, 10000, true);
        motion.Update(9.0f, 0.0f, 20000, true);
        ExpectDelta("disable stops immediately",
                    motion.Update(9.0f, 0.0f, 30000, false),
                    0,
                    0);
        ExpectDelta("re-enable primes instead of replaying remainder",
                    motion.Update(9.0f, 0.0f, 40000, true),
                    0,
                    0);
    }

    return 0;
}
