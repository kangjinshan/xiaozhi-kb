#ifndef MIC_STEREO_PROCESSOR_H_
#define MIC_STEREO_PROCESSOR_H_

#include "recorder_rate_converter.h"

#include <cstdint>
#include <vector>

// Converts interleaved ES7210 MIC1/MIC2 PCM to 16 kHz stereo. One ESP-SR NS
// instance processes the common (mid) signal; its correction is then applied
// equally to both physical channels so their side signal and L/R separation
// remain intact. This is fast enough for the single-core ESP32-C6.
class MicStereoProcessor {
public:
    using FrameProcessor = void (*)(void* context,
                                    int16_t* input,
                                    int16_t* output);

    explicit MicStereoProcessor(int input_sample_rate,
                                FrameProcessor processor = nullptr,
                                void* processor_context = nullptr,
                                bool low_cpu_realtime_mode = false);
    ~MicStereoProcessor();

    MicStereoProcessor(const MicStereoProcessor&) = delete;
    MicStereoProcessor& operator=(const MicStereoProcessor&) = delete;

    bool Process(const std::vector<int16_t>& interleaved_input,
                 std::vector<int16_t>* interleaved_output);
    bool Flush(std::vector<int16_t>* interleaved_output);
    bool Reset();

    bool valid() const { return left_rate_.valid() && right_rate_.valid(); }
    bool noise_reduction_enabled() const { return processor_ != nullptr; }
    int output_sample_rate() const { return 16000; }
    int output_channels() const { return 2; }

private:
    RecorderRateConverter left_rate_;
    RecorderRateConverter right_rate_;
    FrameProcessor processor_ = nullptr;
    void* processor_context_ = nullptr;
    bool low_cpu_realtime_mode_ = false;
    bool process_next_frame_ = true;
    int held_gain_q15_ = 32768;
#if defined(ESP_PLATFORM)
    bool owns_context_ = false;
#endif
    std::vector<int16_t> pending_left_;
    std::vector<int16_t> pending_right_;

    void ProcessCompleteFrames(std::vector<int16_t>* output);
    void ProcessFrame(const int16_t* left,
                      const int16_t* right,
                      int16_t* output);
    static int16_t Limit(int16_t sample);
};

#endif  // MIC_STEREO_PROCESSOR_H_
