#ifndef AIR_MOUSE_MOTION_H_
#define AIR_MOUSE_MOTION_H_

#include "one_euro_filter.h"

#include <cstdint>

struct AirMouseMotionConfig {
    float min_cutoff_hz = 1.2f;
    float beta = 0.025f;
    float derivative_cutoff_hz = 1.0f;
    float deadzone_dps = 0.45f;
    float pixels_per_degree = 12.0f;
    float acceleration_start_dps = 24.0f;
    float acceleration_per_dps = 0.028f;
    float maximum_gain = 2.8f;
    float maximum_elapsed_seconds = 0.04f;
};

struct AirMouseDelta {
    int8_t x = 0;
    int8_t y = 0;

    bool HasMovement() const { return x != 0 || y != 0; }
};

// Converts already axis-mapped gyro rates into relative HID mouse deltas.
// The caller owns board-axis/sign selection; horizontal_dps > 0 means right
// and vertical_dps > 0 means down.
class AirMouseMotion {
public:
    explicit AirMouseMotion(
        const AirMouseMotionConfig& config = AirMouseMotionConfig());

    AirMouseDelta Update(float horizontal_dps,
                         float vertical_dps,
                         uint64_t timestamp_us,
                         bool enabled);
    void Reset();

private:
    static float ApplyDeadzone(float value, float deadzone);
    static int8_t TakeWholePixels(float* accumulator);

    AirMouseMotionConfig config_;
    OneEuroFilter horizontal_filter_;
    OneEuroFilter vertical_filter_;
    bool active_ = false;
    uint64_t previous_timestamp_us_ = 0;
    float x_remainder_ = 0.0f;
    float y_remainder_ = 0.0f;
};

#endif  // AIR_MOUSE_MOTION_H_
