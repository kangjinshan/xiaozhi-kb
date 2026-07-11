#include "recorder_noise_reducer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void ExpectTrue(const char* name, bool value) {
    if (!value) {
        std::fprintf(stderr, "%s: expected true\n", name);
        std::exit(1);
    }
}

static void ExpectSize(const char* name, size_t actual, size_t expected) {
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %zu, got %zu\n", name, expected, actual);
        std::exit(1);
    }
}

static int16_t MaxAbs(const std::vector<int16_t>& data, size_t begin, size_t end) {
    int max_abs = 0;
    for (size_t i = begin; i < end; ++i) {
        int sample = data[i];
        int abs_sample = sample < 0 ? -sample : sample;
        max_abs = std::max(max_abs, abs_sample);
    }
    return static_cast<int16_t>(max_abs);
}

int main() {
    constexpr int kSampleRate = 24000;
    constexpr size_t kFrameSamples = 240;  // 10 ms at 24 kHz

    {
        RecorderNoiseReducer reducer(kSampleRate);
        std::vector<int16_t> samples(kFrameSamples * 2);
        std::fill(samples.begin(), samples.begin() + kFrameSamples, 20);
        std::fill(samples.begin() + kFrameSamples, samples.end(), 600);

        reducer.Process(samples);
        reducer.Flush(samples);

        int16_t quiet_peak = MaxAbs(samples, 0, kFrameSamples);
        int16_t speech_peak = MaxAbs(samples, kFrameSamples, samples.size());

        ExpectTrue("quiet floor remains controlled", quiet_peak <= 120);
        ExpectTrue("speech remains audible", speech_peak >= 2400);
        ExpectTrue("speech dominates quiet floor", speech_peak >= quiet_peak * 20);
    }

    {
        RecorderNoiseReducer reducer(kSampleRate);
        std::vector<int16_t> samples(kFrameSamples, 6000);

        reducer.Process(samples);
        reducer.Flush(samples);

        int16_t peak = MaxAbs(samples, 0, samples.size());
        ExpectTrue("limiter prevents int16 overflow", peak <= 30000);
    }

    {
        RecorderNoiseReducer reducer(kSampleRate);
        std::vector<int16_t> first(123, 400);
        std::vector<int16_t> second(456, 500);
        size_t expected = first.size() + second.size();

        reducer.Process(first);
        reducer.Process(second);
        first.insert(first.end(), second.begin(), second.end());
        reducer.Flush(first);

        ExpectSize("streaming preserves sample count", first.size(), expected);
    }

    return 0;
}
