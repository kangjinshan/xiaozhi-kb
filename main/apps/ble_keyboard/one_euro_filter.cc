#include "one_euro_filter.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kMinimumElapsedSeconds = 0.000001f;
constexpr float kMinimumCutoffHz = 0.000001f;

}  // namespace

OneEuroFilter::OneEuroFilter(float min_cutoff_hz,
                             float beta,
                             float derivative_cutoff_hz)
    : min_cutoff_hz_(std::max(min_cutoff_hz, kMinimumCutoffHz)),
      beta_(std::max(beta, 0.0f)),
      derivative_cutoff_hz_(std::max(derivative_cutoff_hz, kMinimumCutoffHz)) {}

float OneEuroFilter::LowPassFilter::Filter(float input, float alpha) {
    if (!initialized) {
        Reset(input);
        return value;
    }
    value = alpha * input + (1.0f - alpha) * value;
    return value;
}

void OneEuroFilter::LowPassFilter::Reset() {
    initialized = false;
    value = 0.0f;
}

void OneEuroFilter::LowPassFilter::Reset(float input) {
    initialized = true;
    value = input;
}

float OneEuroFilter::Alpha(float cutoff_hz, float elapsed_seconds) {
    const float safe_elapsed = std::max(elapsed_seconds, kMinimumElapsedSeconds);
    const float safe_cutoff = std::max(cutoff_hz, kMinimumCutoffHz);
    const float tau = 1.0f / (kTwoPi * safe_cutoff);
    return 1.0f / (1.0f + tau / safe_elapsed);
}

float OneEuroFilter::Filter(float value, float elapsed_seconds) {
    float derivative = 0.0f;
    if (has_previous_raw_) {
        derivative = (value - previous_raw_) /
                     std::max(elapsed_seconds, kMinimumElapsedSeconds);
    }
    previous_raw_ = value;
    has_previous_raw_ = true;

    const float filtered_derivative = derivative_filter_.Filter(
        derivative, Alpha(derivative_cutoff_hz_, elapsed_seconds));
    const float adaptive_cutoff =
        min_cutoff_hz_ + beta_ * std::fabs(filtered_derivative);
    return signal_filter_.Filter(value, Alpha(adaptive_cutoff, elapsed_seconds));
}

void OneEuroFilter::Reset() {
    has_previous_raw_ = false;
    previous_raw_ = 0.0f;
    signal_filter_.Reset();
    derivative_filter_.Reset();
}

void OneEuroFilter::Reset(float value) {
    has_previous_raw_ = true;
    previous_raw_ = value;
    signal_filter_.Reset(value);
    derivative_filter_.Reset(0.0f);
}
