#include "recorder_rate_converter.h"

#include <algorithm>
#include <limits>

#if defined(ESP_PLATFORM)
#include "esp_ae_rate_cvt.h"
#endif

RecorderRateConverter::RecorderRateConverter(int source_rate, int destination_rate)
    : source_rate_(source_rate), destination_rate_(destination_rate) {
    if (source_rate_ <= 0 || destination_rate_ <= 0) {
        return;
    }

#if defined(ESP_PLATFORM)
    if (source_rate_ != destination_rate_) {
        esp_ae_rate_cvt_cfg_t config = {
            .src_rate = static_cast<uint32_t>(source_rate_),
            .dest_rate = static_cast<uint32_t>(destination_rate_),
            .channel = 1,
            .bits_per_sample = 16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
        };
        if (esp_ae_rate_cvt_open(&config, &handle_) != ESP_AE_ERR_OK || handle_ == nullptr) {
            handle_ = nullptr;
            return;
        }
    }
#endif
    valid_ = true;
}

RecorderRateConverter::~RecorderRateConverter() {
#if defined(ESP_PLATFORM)
    if (handle_ != nullptr) {
        esp_ae_rate_cvt_close(handle_);
        handle_ = nullptr;
    }
#endif
}

uint64_t RecorderRateConverter::ExpectedOutputSamples() const {
    return total_input_samples_ * static_cast<uint64_t>(destination_rate_) /
           static_cast<uint64_t>(source_rate_);
}

void RecorderRateConverter::ReleaseExpected(std::vector<int16_t>* output) {
    const uint64_t target = ExpectedOutputSamples();
    const uint64_t wanted = target - emitted_output_samples_;
    const size_t count = static_cast<size_t>(
        std::min<uint64_t>(wanted, pending_output_.size()));
    output->insert(output->end(), pending_output_.begin(), pending_output_.begin() + count);
    pending_output_.erase(pending_output_.begin(), pending_output_.begin() + count);
    emitted_output_samples_ += count;
}

#if defined(ESP_PLATFORM)
bool RecorderRateConverter::ConvertDeviceBlock(const int16_t* input, size_t samples) {
    if (samples == 0) {
        return true;
    }
    if (input == nullptr || samples > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    if (source_rate_ == destination_rate_) {
        pending_output_.insert(pending_output_.end(), input, input + samples);
        generated_output_samples_ += samples;
        return true;
    }
    if (handle_ == nullptr) {
        return false;
    }

    uint32_t capacity = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(
            handle_, static_cast<uint32_t>(samples), &capacity) != ESP_AE_ERR_OK ||
        capacity == 0) {
        return false;
    }
    std::vector<int16_t> converted(capacity);
    uint32_t actual = capacity;
    if (esp_ae_rate_cvt_process(
            handle_,
            reinterpret_cast<esp_ae_sample_t>(const_cast<int16_t*>(input)),
            static_cast<uint32_t>(samples),
            reinterpret_cast<esp_ae_sample_t>(converted.data()),
            &actual) != ESP_AE_ERR_OK ||
        actual > capacity) {
        return false;
    }
    pending_output_.insert(pending_output_.end(), converted.begin(), converted.begin() + actual);
    generated_output_samples_ += actual;
    return true;
}
#else
void RecorderRateConverter::GenerateHostOutput() {
    const uint64_t target = ExpectedOutputSamples();
    while (generated_output_samples_ < target) {
        const uint64_t source_index = generated_output_samples_ *
            static_cast<uint64_t>(source_rate_) /
            static_cast<uint64_t>(destination_rate_);
        const size_t local = static_cast<size_t>(source_index - host_input_base_);
        pending_output_.push_back(host_input_.at(local));
        ++generated_output_samples_;
    }

    const uint64_t next_source = generated_output_samples_ *
        static_cast<uint64_t>(source_rate_) /
        static_cast<uint64_t>(destination_rate_);
    const size_t discard = static_cast<size_t>(
        std::min<uint64_t>(next_source - host_input_base_, host_input_.size()));
    host_input_.erase(host_input_.begin(), host_input_.begin() + discard);
    host_input_base_ += discard;
}
#endif

bool RecorderRateConverter::Process(const std::vector<int16_t>& input,
                                    std::vector<int16_t>* output) {
    if (!valid_ || flushed_ || output == nullptr) {
        return false;
    }

#if !defined(ESP_PLATFORM)
    host_input_.insert(host_input_.end(), input.begin(), input.end());
#endif
    total_input_samples_ += input.size();

#if defined(ESP_PLATFORM)
    if (!ConvertDeviceBlock(input.data(), input.size())) {
        return false;
    }
#else
    GenerateHostOutput();
#endif
    ReleaseExpected(output);
    return true;
}

bool RecorderRateConverter::Flush(std::vector<int16_t>* output) {
    if (!valid_ || flushed_ || output == nullptr) {
        return false;
    }
    flushed_ = true;

#if !defined(ESP_PLATFORM)
    GenerateHostOutput();
#endif
    ReleaseExpected(output);

#if defined(ESP_PLATFORM)
    std::vector<int16_t> zeros(256, 0);
    for (int attempt = 0;
         emitted_output_samples_ < ExpectedOutputSamples() && attempt < 8;
         ++attempt) {
        const uint64_t generated_before = generated_output_samples_;
        if (!ConvertDeviceBlock(zeros.data(), zeros.size())) {
            return false;
        }
        ReleaseExpected(output);
        if (generated_output_samples_ == generated_before) {
            break;
        }
    }
#endif
    return emitted_output_samples_ == ExpectedOutputSamples();
}

bool RecorderRateConverter::Reset() {
    if (!valid_) {
        return false;
    }
#if defined(ESP_PLATFORM)
    if (handle_ != nullptr && esp_ae_rate_cvt_reset(handle_) != ESP_AE_ERR_OK) {
        return false;
    }
#else
    host_input_base_ = 0;
    host_input_.clear();
#endif
    flushed_ = false;
    total_input_samples_ = 0;
    emitted_output_samples_ = 0;
    generated_output_samples_ = 0;
    pending_output_.clear();
    return true;
}
