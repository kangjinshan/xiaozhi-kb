#ifndef RECORDER_RATE_CONVERTER_H_
#define RECORDER_RATE_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

class RecorderRateConverter {
public:
    RecorderRateConverter(int source_rate, int destination_rate);
    ~RecorderRateConverter();

    RecorderRateConverter(const RecorderRateConverter&) = delete;
    RecorderRateConverter& operator=(const RecorderRateConverter&) = delete;

    bool valid() const { return valid_; }
    int source_rate() const { return source_rate_; }
    int destination_rate() const { return destination_rate_; }
    bool Process(const std::vector<int16_t>& input, std::vector<int16_t>* output);
    bool Flush(std::vector<int16_t>* output);
    bool Reset();

private:
    int source_rate_;
    int destination_rate_;
    bool valid_ = false;
    bool flushed_ = false;
    uint64_t total_input_samples_ = 0;
    uint64_t emitted_output_samples_ = 0;
    uint64_t generated_output_samples_ = 0;
    std::vector<int16_t> pending_output_;

#if defined(ESP_PLATFORM)
    void* handle_ = nullptr;
    bool ConvertDeviceBlock(const int16_t* input, size_t samples);
#else
    uint64_t host_input_base_ = 0;
    std::vector<int16_t> host_input_;
    void GenerateHostOutput();
#endif

    uint64_t ExpectedOutputSamples() const;
    void ReleaseExpected(std::vector<int16_t>* output);
};

#endif  // RECORDER_RATE_CONVERTER_H_
