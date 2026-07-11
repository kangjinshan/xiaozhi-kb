#ifndef RECORDER_NOISE_REDUCER_H_
#define RECORDER_NOISE_REDUCER_H_

#include "recorder_rate_converter.h"

#include <cstdint>
#include <vector>

class RecorderNoiseReducer {
public:
    using FrameProcessor = void (*)(void* context, int16_t* input, int16_t* output);

    explicit RecorderNoiseReducer(int input_sample_rate,
                                  FrameProcessor processor = nullptr,
                                  void* processor_context = nullptr);
    ~RecorderNoiseReducer();

    RecorderNoiseReducer(const RecorderNoiseReducer&) = delete;
    RecorderNoiseReducer& operator=(const RecorderNoiseReducer&) = delete;

    bool Process(const std::vector<int16_t>& input, std::vector<int16_t>* output);
    bool Flush(std::vector<int16_t>* output);
    bool Reset();
    bool valid() const { return rate_converter_.valid(); }
    bool noise_reduction_enabled() const { return frame_processor_ != nullptr; }
    int output_sample_rate() const { return 16000; }

private:
    RecorderRateConverter rate_converter_;
    FrameProcessor frame_processor_ = nullptr;
    void* processor_context_ = nullptr;
    bool owns_processor_context_ = false;
    std::vector<int16_t> pending_16k_;

    void ProcessCompleteFrames(std::vector<int16_t>* output);
    void ProcessOneFrame(int16_t* input, int16_t* output);
    static int16_t ApplyGainAndLimit(int16_t sample);
};

#endif  // RECORDER_NOISE_REDUCER_H_
