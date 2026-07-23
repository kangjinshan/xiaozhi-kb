#include "touchpad_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

constexpr int kHidRelativeMinimum = -127;
constexpr int kHidRelativeMaximum = 127;

}  // namespace

TouchpadMotion::TouchpadMotion(const TouchpadMotionConfig& config)
    : config_(config) {}

TouchpadDelta TouchpadMotion::Update(uint16_t x,
                                     uint16_t y,
                                     bool touched,
                                     bool enabled) {
    if (!enabled || !touched) {
        Reset();
        return {};
    }

    if (!tracking_) {
        tracking_ = true;
        previous_x_ = x;
        previous_y_ = y;
        return {};
    }

    const int delta_x =
        static_cast<int>(x) - static_cast<int>(previous_x_);
    const int delta_y =
        static_cast<int>(y) - static_cast<int>(previous_y_);
    previous_x_ = x;
    previous_y_ = y;

    if (std::abs(delta_x) >
            static_cast<int>(config_.maximum_sample_delta) ||
        std::abs(delta_y) >
            static_cast<int>(config_.maximum_sample_delta)) {
        x_remainder_ = 0.0f;
        y_remainder_ = 0.0f;
        return {};
    }

    const int speed = std::max(std::abs(delta_x), std::abs(delta_y));
    const int accelerated_pixels = std::max(
        speed - static_cast<int>(config_.acceleration_start_pixels), 0);
    const float gain = std::min(
        config_.base_gain +
            static_cast<float>(accelerated_pixels) *
                config_.acceleration_per_pixel,
        config_.maximum_gain);

    x_remainder_ += static_cast<float>(delta_x) * gain;
    y_remainder_ += static_cast<float>(delta_y) * gain;

    return {
        .x = TakeWholePixels(&x_remainder_),
        .y = TakeWholePixels(&y_remainder_),
    };
}

void TouchpadMotion::Reset() {
    tracking_ = false;
    previous_x_ = 0;
    previous_y_ = 0;
    x_remainder_ = 0.0f;
    y_remainder_ = 0.0f;
}

int8_t TouchpadMotion::TakeWholePixels(float* accumulator) {
    const int whole_pixels = static_cast<int>(std::trunc(*accumulator));
    if (whole_pixels > kHidRelativeMaximum) {
        *accumulator = 0.0f;
        return static_cast<int8_t>(kHidRelativeMaximum);
    }
    if (whole_pixels < kHidRelativeMinimum) {
        *accumulator = 0.0f;
        return static_cast<int8_t>(kHidRelativeMinimum);
    }

    *accumulator -= static_cast<float>(whole_pixels);
    return static_cast<int8_t>(whole_pixels);
}
