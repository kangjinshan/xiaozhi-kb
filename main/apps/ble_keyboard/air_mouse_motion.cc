#include "air_mouse_motion.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMicrosToSeconds = 0.000001f;
constexpr int kHidRelativeMinimum = -127;
constexpr int kHidRelativeMaximum = 127;

}  // namespace

AirMouseMotion::AirMouseMotion(const AirMouseMotionConfig& config)
    : config_(config),
      horizontal_filter_(
          config.min_cutoff_hz, config.beta, config.derivative_cutoff_hz),
      vertical_filter_(
          config.min_cutoff_hz, config.beta, config.derivative_cutoff_hz) {}

AirMouseDelta AirMouseMotion::Update(float horizontal_dps,
                                     float vertical_dps,
                                     uint64_t timestamp_us,
                                     bool enabled) {
    if (!enabled) {
        Reset();
        return {};
    }

    if (!active_) {
        active_ = true;
        previous_timestamp_us_ = timestamp_us;
        horizontal_filter_.Reset(horizontal_dps);
        vertical_filter_.Reset(vertical_dps);
        return {};
    }

    if (timestamp_us <= previous_timestamp_us_) {
        return {};
    }

    const float elapsed_seconds =
        static_cast<float>(timestamp_us - previous_timestamp_us_) *
        kMicrosToSeconds;
    previous_timestamp_us_ = timestamp_us;
    if (elapsed_seconds > config_.maximum_elapsed_seconds) {
        horizontal_filter_.Reset(horizontal_dps);
        vertical_filter_.Reset(vertical_dps);
        x_remainder_ = 0.0f;
        y_remainder_ = 0.0f;
        return {};
    }

    const float filtered_x =
        ApplyDeadzone(horizontal_filter_.Filter(horizontal_dps, elapsed_seconds),
                      config_.deadzone_dps);
    const float filtered_y =
        ApplyDeadzone(vertical_filter_.Filter(vertical_dps, elapsed_seconds),
                      config_.deadzone_dps);

    const float angular_speed =
        std::sqrt(filtered_x * filtered_x + filtered_y * filtered_y);
    const float extra_speed =
        std::max(angular_speed - config_.acceleration_start_dps, 0.0f);
    const float gain = config_.pixels_per_degree *
                       std::min(1.0f + extra_speed *
                                          config_.acceleration_per_dps,
                                config_.maximum_gain);

    x_remainder_ += filtered_x * elapsed_seconds * gain;
    y_remainder_ += filtered_y * elapsed_seconds * gain;

    return {
        .x = TakeWholePixels(&x_remainder_),
        .y = TakeWholePixels(&y_remainder_),
    };
}

void AirMouseMotion::Reset() {
    active_ = false;
    previous_timestamp_us_ = 0;
    x_remainder_ = 0.0f;
    y_remainder_ = 0.0f;
    horizontal_filter_.Reset();
    vertical_filter_.Reset();
}

float AirMouseMotion::ApplyDeadzone(float value, float deadzone) {
    const float magnitude = std::fabs(value);
    if (magnitude <= deadzone) {
        return 0.0f;
    }
    return std::copysign(magnitude - deadzone, value);
}

int8_t AirMouseMotion::TakeWholePixels(float* accumulator) {
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
