#ifndef RECORDER_AMR_ENCODER_H_
#define RECORDER_AMR_ENCODER_H_
#include "recorder_rate_converter.h"
#include <functional>
#include <vector>

// Process denoised 16 kHz mono PCM incrementally into AMR-NB 8 kHz / 12.2 kbps.
// The caller writes the six-byte AMR file header once. Each encoded frame is 20 ms.
class RecorderAmrEncoder {
public:
    using FrameEncoder = std::function<bool(const int16_t*, std::vector<uint8_t>*)>;
    explicit RecorderAmrEncoder(FrameEncoder test_encoder = {});
    ~RecorderAmrEncoder();
    bool valid() const;
    bool Reset();
    bool Process(const std::vector<int16_t>& pcm, std::vector<uint8_t>* output);
    bool Flush(std::vector<uint8_t>* output);
private:
    bool EncodePending(std::vector<uint8_t>* output);
    RecorderRateConverter rate_{16000, 8000};
    std::vector<int16_t> pending_;
    FrameEncoder test_encoder_;
    void* encoder_ = nullptr;
    bool flushed_ = false;
};
#endif
