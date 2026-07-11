#include "recorder_noise_reducer.h"

#include <algorithm>

#if defined(ESP_PLATFORM)
#include <cmath>
#include <esp_log.h>
#include "esp_agc.h"
#include "esp_ns.h"
#endif

namespace {

#if defined(ESP_PLATFORM)
constexpr const char* kTag = "recorder_ns";
#endif
constexpr int kProcessingSampleRate = 16000;
constexpr int kFrameDurationMs = 10;
constexpr int kFallbackNoiseGate = 45;
constexpr int kFallbackGain = 6;
constexpr int kLimiterPeak = 30000;

int16_t ClampSample(int value) {
    if (value > kLimiterPeak) {
        return kLimiterPeak;
    }
    if (value < -kLimiterPeak) {
        return -kLimiterPeak;
    }
    return static_cast<int16_t>(value);
}

#if defined(ESP_PLATFORM)
std::vector<int16_t> ResampleLinear(const int16_t* input,
                                    size_t input_samples,
                                    int input_rate,
                                    int output_rate) {
    if (input == nullptr || input_samples == 0 || input_rate <= 0 || output_rate <= 0) {
        return {};
    }
    if (input_rate == output_rate) {
        return std::vector<int16_t>(input, input + input_samples);
    }

    size_t output_samples = (input_samples * static_cast<size_t>(output_rate) +
                             static_cast<size_t>(input_rate) - 1) /
                            static_cast<size_t>(input_rate);
    output_samples = std::max<size_t>(1, output_samples);

    std::vector<int16_t> output(output_samples);
    if (output_samples == 1 || input_samples == 1) {
        std::fill(output.begin(), output.end(), input[0]);
        return output;
    }

    const double scale = static_cast<double>(input_samples - 1) /
                         static_cast<double>(output_samples - 1);
    for (size_t i = 0; i < output_samples; ++i) {
        const double src = static_cast<double>(i) * scale;
        const size_t left = static_cast<size_t>(src);
        const size_t right = std::min(left + 1, input_samples - 1);
        const double frac = src - static_cast<double>(left);
        const double sample = static_cast<double>(input[left]) * (1.0 - frac) +
                              static_cast<double>(input[right]) * frac;
        output[i] = ClampSample(static_cast<int>(std::lround(sample)));
    }
    return output;
}
#endif

}  // namespace

RecorderNoiseReducer::RecorderNoiseReducer(int sample_rate)
    : sample_rate_(sample_rate > 0 ? sample_rate : kProcessingSampleRate),
      frame_samples_(static_cast<size_t>(std::max(1, sample_rate_ / (1000 / kFrameDurationMs)))) {
#if defined(ESP_PLATFORM)
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    ESP_LOGW(kTag, "ESP-SR NS disabled on ESP32-C6, using fallback enhancer");
#else
    ns_handle_ = ns_pro_create(kFrameDurationMs, 2, kProcessingSampleRate);
    agc_handle_ = esp_agc_open(AGC_MODE_2, kProcessingSampleRate);
    if (agc_handle_ != nullptr) {
        set_agc_config(agc_handle_, 9, 1, -6);
    }
    noise_reduction_enabled_ = ns_handle_ != nullptr;
    if (noise_reduction_enabled_) {
        ESP_LOGI(kTag, "ESP-SR NS%s enabled, input=%d Hz, process=%d Hz",
                 agc_handle_ != nullptr ? "/AGC" : "",
                 sample_rate_, kProcessingSampleRate);
    } else {
        ESP_LOGW(kTag, "ESP-SR NS init failed, using fallback enhancer");
    }
#endif
#endif
}

RecorderNoiseReducer::~RecorderNoiseReducer() {
#if defined(ESP_PLATFORM)
    if (agc_handle_ != nullptr) {
        esp_agc_close(agc_handle_);
        agc_handle_ = nullptr;
    }
    if (ns_handle_ != nullptr) {
        ns_destroy(ns_handle_);
        ns_handle_ = nullptr;
    }
#endif
}

void RecorderNoiseReducer::Process(std::vector<int16_t>& samples) {
    if (samples.empty()) {
        return;
    }

    if (noise_reduction_enabled_) {
        ProcessWithEspSr(samples);
        ApplyLimiter(samples);
    } else {
        ApplyFallbackEnhancement(samples);
    }
}

void RecorderNoiseReducer::Flush(std::vector<int16_t>& samples) {
    ApplyLimiter(samples);
}

void RecorderNoiseReducer::ProcessWithEspSr(std::vector<int16_t>& samples) {
#if defined(ESP_PLATFORM)
    if (ns_handle_ == nullptr) {
        ApplyFallbackEnhancement(samples);
        return;
    }

    size_t offset = 0;
    while (offset + frame_samples_ <= samples.size()) {
        std::vector<int16_t> frame_16k =
            ResampleLinear(samples.data() + offset, frame_samples_, sample_rate_, kProcessingSampleRate);
        std::vector<int16_t> ns_output(frame_16k.size());
        ns_process(ns_handle_, frame_16k.data(), ns_output.data());

        const int frame_size = static_cast<int>(ns_output.size());
        if (agc_handle_ != nullptr &&
            esp_agc_process(agc_handle_, ns_output.data(), frame_16k.data(),
                            frame_size, kProcessingSampleRate) == ESP_AGC_SUCCESS) {
            ns_output = frame_16k;
        }

        std::vector<int16_t> back_to_input_rate =
            ResampleLinear(ns_output.data(), ns_output.size(),
                           kProcessingSampleRate, sample_rate_);
        const size_t copy_count = std::min(frame_samples_, back_to_input_rate.size());
        std::copy(back_to_input_rate.begin(),
                  back_to_input_rate.begin() + static_cast<std::ptrdiff_t>(copy_count),
                  samples.begin() + static_cast<std::ptrdiff_t>(offset));
        if (copy_count < frame_samples_) {
            std::fill(samples.begin() + static_cast<std::ptrdiff_t>(offset + copy_count),
                      samples.begin() + static_cast<std::ptrdiff_t>(offset + frame_samples_),
                      0);
        }
        offset += frame_samples_;
    }

    if (offset < samples.size()) {
        for (size_t i = offset; i < samples.size(); ++i) {
            samples[i] = ClampSample(static_cast<int>(samples[i]) * kFallbackGain);
        }
    }
#else
    ApplyFallbackEnhancement(samples);
#endif
}

void RecorderNoiseReducer::ApplyFallbackEnhancement(std::vector<int16_t>& samples) {
    for (auto& sample : samples) {
        const int value = sample;
        const int abs_value = value < 0 ? -value : value;
        if (abs_value <= kFallbackNoiseGate) {
            sample = 0;
            continue;
        }
        sample = ClampSample(value * kFallbackGain);
    }
}

void RecorderNoiseReducer::ApplyLimiter(std::vector<int16_t>& samples) {
    for (auto& sample : samples) {
        sample = ClampSample(sample);
    }
}
