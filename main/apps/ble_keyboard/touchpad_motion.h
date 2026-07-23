#ifndef TOUCHPAD_MOTION_H_
#define TOUCHPAD_MOTION_H_

#include <cstdint>

struct TouchpadMotionConfig {
    float base_gain = 1.25f;
    uint16_t acceleration_start_pixels = 4;
    float acceleration_per_pixel = 0.08f;
    float maximum_gain = 2.5f;
    uint16_t maximum_sample_delta = 160;
};

struct TouchpadDelta {
    int8_t x = 0;
    int8_t y = 0;

    bool HasMovement() const { return x != 0 || y != 0; }
};

// Converts calibrated absolute touch coordinates into relative HID mouse
// movement. A new contact only establishes an anchor, so tapping never moves
// or clicks the pointer. Slow drags remain precise while faster drags gain
// bounded acceleration.
class TouchpadMotion {
public:
    explicit TouchpadMotion(
        const TouchpadMotionConfig& config = TouchpadMotionConfig());

    TouchpadDelta Update(uint16_t x,
                         uint16_t y,
                         bool touched,
                         bool enabled);
    void Reset();

private:
    static int8_t TakeWholePixels(float* accumulator);

    TouchpadMotionConfig config_;
    bool tracking_ = false;
    uint16_t previous_x_ = 0;
    uint16_t previous_y_ = 0;
    float x_remainder_ = 0.0f;
    float y_remainder_ = 0.0f;
};

#endif  // TOUCHPAD_MOTION_H_
