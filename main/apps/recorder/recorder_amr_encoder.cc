#include "recorder_amr_encoder.h"
#include "recorder_amr_frame.h"
#include <utility>
#ifdef ESP_PLATFORM
#include "esp_amrnb_enc.h"
#endif

RecorderAmrEncoder::RecorderAmrEncoder(FrameEncoder test_encoder)
    : test_encoder_(std::move(test_encoder)) {
#ifdef ESP_PLATFORM
    if (!test_encoder_) {
        esp_amrnb_enc_config_t config = ESP_AMRNB_ENC_CONFIG_DEFAULT();
        config.no_file_header = true;
        if (esp_amrnb_enc_open(&config, sizeof(config), &encoder_) != ESP_AUDIO_ERR_OK) encoder_ = nullptr;
    }
#endif
}
RecorderAmrEncoder::~RecorderAmrEncoder() {
#ifdef ESP_PLATFORM
    if (encoder_) esp_amrnb_enc_close(encoder_);
#endif
}
bool RecorderAmrEncoder::valid() const { return rate_.valid() && (encoder_ || test_encoder_); }
bool RecorderAmrEncoder::Reset() {
    pending_.clear(); flushed_ = false;
    if (!valid() || !rate_.Reset()) return false;
#ifdef ESP_PLATFORM
    if (encoder_ && esp_amrnb_enc_reset(encoder_) != ESP_AUDIO_ERR_OK) return false;
#endif
    return true;
}
bool RecorderAmrEncoder::EncodePending(std::vector<uint8_t>* output) {
    while (pending_.size() >= 160) {
        if (test_encoder_) {
            if (!test_encoder_(pending_.data(), output)) return false;
        } else {
#ifdef ESP_PLATFORM
            uint8_t encoded[64];
            esp_audio_enc_in_frame_t input{};
            input.buffer = reinterpret_cast<uint8_t*>(pending_.data()); input.len = 320;
            esp_audio_enc_out_frame_t frame{};
            frame.buffer = encoded; frame.len = sizeof(encoded);
            if (esp_amrnb_enc_process(encoder_, &input, &frame) != ESP_AUDIO_ERR_OK ||
                frame.encoded_bytes != 32 || RecorderAmrFrameSize(encoded[0]) != 32) return false;
            output->insert(output->end(), encoded, encoded + frame.encoded_bytes);
#else
            return false;
#endif
        }
        pending_.erase(pending_.begin(), pending_.begin() + 160);
    }
    return true;
}
bool RecorderAmrEncoder::Process(const std::vector<int16_t>& pcm, std::vector<uint8_t>* output) {
    if (!output || !valid() || flushed_) return false;
    if (!rate_.Process(pcm, &pending_)) return false;
    return EncodePending(output);
}
bool RecorderAmrEncoder::Flush(std::vector<uint8_t>* output) {
    if (!output || !valid() || flushed_) return false;
    flushed_ = true;
    if (!rate_.Flush(&pending_)) return false;
    if (!pending_.empty()) pending_.resize(((pending_.size() + 159) / 160) * 160, 0);
    return EncodePending(output);
}
