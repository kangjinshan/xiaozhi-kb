#ifndef RECORDER_NOISE_REDUCER_H
#define RECORDER_NOISE_REDUCER_H

#include <cstddef>
#include <cstdint>
#include <vector>

class RecorderNoiseReducer {
public:
    explicit RecorderNoiseReducer(int sample_rate);
    ~RecorderNoiseReducer();

    RecorderNoiseReducer(const RecorderNoiseReducer&) = delete;
    RecorderNoiseReducer& operator=(const RecorderNoiseReducer&) = delete;

    void Process(std::vector<int16_t>& samples);
    void Flush(std::vector<int16_t>& samples);

    bool noise_reduction_enabled() const { return noise_reduction_enabled_; }

private:
    int sample_rate_;
    size_t frame_samples_;
    bool noise_reduction_enabled_ = false;
#if defined(ESP_PLATFORM)
    void* ns_handle_ = nullptr;
    void* agc_handle_ = nullptr;
#endif

    void ProcessWithEspSr(std::vector<int16_t>& samples);
    void ApplyFallbackEnhancement(std::vector<int16_t>& samples);
    void ApplyLimiter(std::vector<int16_t>& samples);
};

#endif  // RECORDER_NOISE_REDUCER_H
