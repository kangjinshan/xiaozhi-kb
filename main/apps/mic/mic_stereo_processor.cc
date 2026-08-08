#include "mic_stereo_processor.h"

#include <algorithm>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ns.h"
#endif

namespace {

constexpr size_t kNsFrameSamples = 160;  // ESP-SR analysis frame: 10 ms
constexpr int kLimiterPeak = 30000;
constexpr int kUnityGainQ15 = 32768;
constexpr int kMinimumNsGainQ15 = 4096;
constexpr int kMaximumNsGainQ15 = 40960;

#if defined(ESP_PLATFORM)
constexpr const char* kTag = "mic_stereo_ns";
#endif

}  // namespace

MicStereoProcessor::MicStereoProcessor(int input_sample_rate,
                                       FrameProcessor processor,
                                       void* processor_context,
                                       bool low_cpu_realtime_mode)
    : left_rate_(input_sample_rate, 16000),
      right_rate_(input_sample_rate, 16000),
      low_cpu_realtime_mode_(low_cpu_realtime_mode) {
    processor_ = processor;
    processor_context_ = processor_context;

#if defined(ESP_PLATFORM)
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "mid/side NS heap before: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
    if (processor_ == nullptr) {
        processor_context_ = ns_create(10);
        if (processor_context_ != nullptr) {
            processor_ = [](void* context, int16_t* input, int16_t* output) {
                ns_process(context, input, output);
            };
            owns_context_ = true;
        }
    }
    ESP_LOGI(kTag, "mid/side NS heap after: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
    ESP_LOGI(kTag,
             "ESP-SR shared-mid stereo NS %s, input=%d Hz output=16000 Hz mode=%s",
             noise_reduction_enabled() ? "enabled" : "unavailable",
             input_sample_rate,
             low_cpu_realtime_mode_ ? "C6 realtime 10ms+held" : "full");
#endif
}

MicStereoProcessor::~MicStereoProcessor() {
#if defined(ESP_PLATFORM)
    if (owns_context_ && processor_context_ != nullptr) {
        ns_destroy(processor_context_);
    }
#endif
}

bool MicStereoProcessor::Process(const std::vector<int16_t>& input,
                                 std::vector<int16_t>* output) {
    if (output == nullptr || !valid() || (input.size() % 2) != 0) {
        return false;
    }

    std::vector<int16_t> left;
    std::vector<int16_t> right;
    left.reserve(input.size() / 2);
    right.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2) {
        left.push_back(input[index]);
        right.push_back(input[index + 1]);
    }

    std::vector<int16_t> converted_left;
    std::vector<int16_t> converted_right;
    if (!left_rate_.Process(left, &converted_left) ||
        !right_rate_.Process(right, &converted_right) ||
        converted_left.size() != converted_right.size()) {
        return false;
    }
    pending_left_.insert(pending_left_.end(),
                         converted_left.begin(), converted_left.end());
    pending_right_.insert(pending_right_.end(),
                          converted_right.begin(), converted_right.end());
    ProcessCompleteFrames(output);
    return true;
}

bool MicStereoProcessor::Flush(std::vector<int16_t>* output) {
    if (output == nullptr || !valid()) {
        return false;
    }
    std::vector<int16_t> converted_left;
    std::vector<int16_t> converted_right;
    if (!left_rate_.Flush(&converted_left) ||
        !right_rate_.Flush(&converted_right) ||
        converted_left.size() != converted_right.size()) {
        return false;
    }
    pending_left_.insert(pending_left_.end(),
                         converted_left.begin(), converted_left.end());
    pending_right_.insert(pending_right_.end(),
                          converted_right.begin(), converted_right.end());
    ProcessCompleteFrames(output);

    if (pending_left_.size() != pending_right_.size()) {
        return false;
    }
    if (!pending_left_.empty()) {
        const size_t valid_samples = pending_left_.size();
        int16_t left_in[kNsFrameSamples] = {};
        int16_t right_in[kNsFrameSamples] = {};
        int16_t stereo_out[kNsFrameSamples * 2] = {};
        std::copy(pending_left_.begin(), pending_left_.end(), left_in);
        std::copy(pending_right_.begin(), pending_right_.end(), right_in);
        ProcessFrame(left_in, right_in, stereo_out);
        output->reserve(output->size() + valid_samples * 2);
        for (size_t index = 0; index < valid_samples; ++index) {
            output->push_back(stereo_out[index * 2]);
            output->push_back(stereo_out[index * 2 + 1]);
        }
        pending_left_.clear();
        pending_right_.clear();
    }
    return true;
}

bool MicStereoProcessor::Reset() {
    pending_left_.clear();
    pending_right_.clear();
    process_next_frame_ = true;
    held_gain_q15_ = kUnityGainQ15;
    return left_rate_.Reset() && right_rate_.Reset();
}

void MicStereoProcessor::ProcessCompleteFrames(std::vector<int16_t>* output) {
    while (pending_left_.size() >= kNsFrameSamples &&
           pending_right_.size() >= kNsFrameSamples) {
        int16_t stereo_out[kNsFrameSamples * 2] = {};
        ProcessFrame(pending_left_.data(), pending_right_.data(), stereo_out);
        output->reserve(output->size() + kNsFrameSamples * 2);
        for (size_t index = 0; index < kNsFrameSamples; ++index) {
            output->push_back(stereo_out[index * 2]);
            output->push_back(stereo_out[index * 2 + 1]);
        }
        pending_left_.erase(pending_left_.begin(),
                            pending_left_.begin() + kNsFrameSamples);
        pending_right_.erase(pending_right_.begin(),
                             pending_right_.begin() + kNsFrameSamples);
    }
}

void MicStereoProcessor::ProcessFrame(const int16_t* left,
                                      const int16_t* right,
                                      int16_t* output) {
    int16_t mid_in[kNsFrameSamples] = {};
    int16_t mid_out[kNsFrameSamples] = {};
    for (size_t index = 0; index < kNsFrameSamples; ++index) {
        mid_in[index] = static_cast<int16_t>(
            (static_cast<int32_t>(left[index]) + right[index]) / 2);
    }
    if (processor_ != nullptr &&
        (!low_cpu_realtime_mode_ || process_next_frame_)) {
        processor_(processor_context_, mid_in, mid_out);
        if (low_cpu_realtime_mode_) {
            uint64_t input_level = 0;
            uint64_t output_level = 0;
            for (size_t index = 0; index < kNsFrameSamples; ++index) {
                input_level += static_cast<uint32_t>(
                    std::abs(static_cast<int>(mid_in[index])));
                output_level += static_cast<uint32_t>(
                    std::abs(static_cast<int>(mid_out[index])));
            }
            if (input_level > 0) {
                held_gain_q15_ = std::clamp(
                    static_cast<int>(
                        output_level * kUnityGainQ15 / input_level),
                    kMinimumNsGainQ15,
                    kMaximumNsGainQ15);
            }
        }
    } else if (processor_ != nullptr) {
        // The ESP32-C6 is single-core and two full 10 ms NS calls plus Wi-Fi
        // exceed a 20 ms deadline. Apply the most recent ESP-SR common-mode
        // attenuation to the alternate frame; the physical L-R side signal
        // is still preserved below.
        for (size_t index = 0; index < kNsFrameSamples; ++index) {
            mid_out[index] = static_cast<int16_t>(std::clamp(
                static_cast<int>(mid_in[index]) * held_gain_q15_ /
                    kUnityGainQ15,
                -32768,
                32767));
        }
    } else {
        std::copy(mid_in, mid_in + kNsFrameSamples, mid_out);
    }
    if (processor_ != nullptr && low_cpu_realtime_mode_) {
        process_next_frame_ = !process_next_frame_;
    }

    for (size_t index = 0; index < kNsFrameSamples; ++index) {
        // Applying the same common-mode correction to both microphones keeps
        // L-R exactly unchanged until the final safety limiter clips a peak.
        const int correction =
            static_cast<int>(mid_out[index]) - mid_in[index];
        output[index * 2] = Limit(static_cast<int16_t>(std::clamp(
            static_cast<int>(left[index]) + correction, -32768, 32767)));
        output[index * 2 + 1] = Limit(static_cast<int16_t>(std::clamp(
            static_cast<int>(right[index]) + correction, -32768, 32767)));
    }
}

int16_t MicStereoProcessor::Limit(int16_t sample) {
    return static_cast<int16_t>(
        std::clamp(static_cast<int>(sample), -kLimiterPeak, kLimiterPeak));
}
