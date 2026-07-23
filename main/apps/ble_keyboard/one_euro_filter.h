#ifndef ONE_EURO_FILTER_H_
#define ONE_EURO_FILTER_H_

#include <cstdint>

// One Euro Filter, adapted from the BSD-3-Clause reference implementation:
// https://github.com/casiez/OneEuroFilterArduino
//
// The filter raises its cutoff while the signal changes quickly, preserving
// responsiveness while suppressing low-speed gyro noise.
class OneEuroFilter {
public:
    OneEuroFilter(float min_cutoff_hz, float beta, float derivative_cutoff_hz);

    float Filter(float value, float elapsed_seconds);
    void Reset();
    void Reset(float value);

private:
    struct LowPassFilter {
        float Filter(float value, float alpha);
        void Reset();
        void Reset(float value);

        bool initialized = false;
        float value = 0.0f;
    };

    static float Alpha(float cutoff_hz, float elapsed_seconds);

    float min_cutoff_hz_;
    float beta_;
    float derivative_cutoff_hz_;
    bool has_previous_raw_ = false;
    float previous_raw_ = 0.0f;
    LowPassFilter signal_filter_;
    LowPassFilter derivative_filter_;
};

#endif  // ONE_EURO_FILTER_H_
