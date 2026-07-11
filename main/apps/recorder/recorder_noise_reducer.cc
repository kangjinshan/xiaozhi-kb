#include "recorder_noise_reducer.h"

#include <algorithm>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ns.h"
#endif

namespace {

constexpr size_t kFrameSamples = 160;
constexpr int kPostGain = 6;
constexpr int kLimiterPeak = 30000;

#if defined(ESP_PLATFORM)
constexpr const char* kTag = "recorder_ns";
#endif

}  // namespace

RecorderNoiseReducer::RecorderNoiseReducer(int input_sample_rate,
                                           FrameProcessor processor,
                                           void* processor_context)
    : rate_converter_(input_sample_rate, 16000),
      frame_processor_(processor),
      processor_context_(processor_context) {
#if defined(ESP_PLATFORM)
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "NS heap before: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
    if (frame_processor_ == nullptr) {
        processor_context_ = ns_create(10);
        owns_processor_context_ = processor_context_ != nullptr;
        if (processor_context_ != nullptr) {
            frame_processor_ = [](void* context, int16_t* input, int16_t* output) {
                ns_process(context, input, output);
            };
        }
    }
    ESP_LOGI(kTag, "NS heap after: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
    ESP_LOGI(kTag, "ESP-SR NS %s, input=%d Hz output=16000 Hz frame=160",
             noise_reduction_enabled() ? "enabled" : "unavailable",
             input_sample_rate);
#endif
}

RecorderNoiseReducer::~RecorderNoiseReducer() {
#if defined(ESP_PLATFORM)
    if (owns_processor_context_) {
        ns_destroy(processor_context_);
    }
#else
    (void)owns_processor_context_;
#endif
}

bool RecorderNoiseReducer::Process(const std::vector<int16_t>& input,
                                   std::vector<int16_t>* output) {
    if (output == nullptr || !valid()) {
        return false;
    }

    std::vector<int16_t> converted;
    if (!rate_converter_.Process(input, &converted)) {
        return false;
    }
    pending_16k_.insert(pending_16k_.end(), converted.begin(), converted.end());
    ProcessCompleteFrames(output);
    return true;
}

bool RecorderNoiseReducer::Flush(std::vector<int16_t>* output) {
    if (output == nullptr || !valid()) {
        return false;
    }

    std::vector<int16_t> converted;
    if (!rate_converter_.Flush(&converted)) {
        return false;
    }
    pending_16k_.insert(pending_16k_.end(), converted.begin(), converted.end());
    ProcessCompleteFrames(output);
    if (!pending_16k_.empty()) {
        const size_t valid_samples = pending_16k_.size();
        int16_t input[kFrameSamples] = {};
        int16_t processed[kFrameSamples] = {};
        std::copy(pending_16k_.begin(), pending_16k_.end(), input);
        ProcessOneFrame(input, processed);
        output->insert(output->end(), processed, processed + valid_samples);
        pending_16k_.clear();
    }
    return true;
}

bool RecorderNoiseReducer::Reset() {
    pending_16k_.clear();
    return rate_converter_.Reset();
}

void RecorderNoiseReducer::ProcessCompleteFrames(std::vector<int16_t>* output) {
    while (pending_16k_.size() >= kFrameSamples) {
        int16_t processed[kFrameSamples] = {};
        ProcessOneFrame(pending_16k_.data(), processed);
        output->insert(output->end(), processed, processed + kFrameSamples);
        pending_16k_.erase(pending_16k_.begin(), pending_16k_.begin() + kFrameSamples);
    }
}

void RecorderNoiseReducer::ProcessOneFrame(int16_t* input, int16_t* output) {
    if (frame_processor_ != nullptr) {
        frame_processor_(processor_context_, input, output);
    } else {
        std::copy(input, input + kFrameSamples, output);
    }
    std::transform(output, output + kFrameSamples, output, ApplyGainAndLimit);
}

int16_t RecorderNoiseReducer::ApplyGainAndLimit(int16_t sample) {
    return static_cast<int16_t>(std::clamp(static_cast<int>(sample) * kPostGain,
                                           -kLimiterPeak,
                                           kLimiterPeak));
}
