#include "recorder_playback_stream.h"
#include <algorithm>
#include <cstring>
#include "recorder_amr_frame.h"
#ifdef ESP_PLATFORM
#include "esp_amrnb_dec.h"
#endif

bool RecorderPlaybackStream::Open(const std::string& path,
                                  int output_rate, int output_channels) {
    Close();
    // Only fully published audio is playable, never a partial download/backup.
    if (path.size() < 4) return false;
    const auto extension = path.substr(path.size() - 4);
    if (extension != ".wav" && extension != ".amr") return false;
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) return false;
    if (extension == ".amr") {
        uint8_t magic[6];
        if (output_channels != 1 || fread(magic, 1, 6, file_) != 6 ||
            std::memcmp(magic, "#!AMR\n", 6) != 0 || fseek(file_, 0, SEEK_END) != 0) {
            Close(); return false;
        }
        const long size = ftell(file_);
        if (size <= 6 || size > 4 * 1024 * 1024 || fseek(file_, 6, SEEK_SET) != 0) {
            Close(); return false;
        }
#ifdef ESP_PLATFORM
        if (esp_amrnb_dec_open(nullptr, 0, &amr_decoder_) != ESP_AUDIO_ERR_OK) {
            Close(); return false;
        }
#else
        Close(); return false; // The target's Espressif decoder is exercised on device.
#endif
        amr_ = true;
        info_ = {8000, 1, 16, 6, static_cast<uint32_t>(size - 6)};
        remaining_ = info_.data_bytes;
        return true;
    }
    uint8_t header[44];
    const bool valid = std::fread(header, 1, sizeof(header), file_) == sizeof(header) &&
        RecorderParseWavHeader(header, sizeof(header), &info_) &&
        RecorderWavCanPlay(info_, output_rate, output_channels) &&
        info_.data_bytes % (info_.channels * sizeof(int16_t)) == 0;
    if (!valid || std::fseek(file_, 0, SEEK_END) != 0 ||
        std::ftell(file_) != static_cast<long>(info_.data_offset + info_.data_bytes) ||
        std::fseek(file_, info_.data_offset, SEEK_SET) != 0) {
        Close();
        return false;
    }
    remaining_ = info_.data_bytes;
    return true;
}

bool RecorderPlaybackStream::Read(std::vector<int16_t>* samples) {
    if (file_ == nullptr || samples == nullptr) return false;
    if (amr_) {
        samples->clear();
        if (remaining_ == 0) return true;
        uint8_t frame[32];
        const int first = fgetc(file_);
        const size_t size = first == EOF ? 0 : RecorderAmrFrameSize(first);
        if (size == 0 || size > remaining_ || fread(frame + 1, 1, size - 1, file_) != size - 1) {
            Close(); return false;
        }
        frame[0] = static_cast<uint8_t>(first);
#ifdef ESP_PLATFORM
        samples->resize(160);
        esp_audio_dec_in_raw_t raw{};
        raw.buffer = frame; raw.len = size;
        esp_audio_dec_out_frame_t output{};
        output.buffer = reinterpret_cast<uint8_t*>(samples->data()); output.len = 320;
        esp_audio_dec_info_t info{};
        if (esp_amrnb_dec_decode(amr_decoder_, &raw, &output, &info) != ESP_AUDIO_ERR_OK ||
            raw.consumed != size || output.decoded_size != 320 ||
            info.sample_rate != 8000 || info.channel != 1 || info.bits_per_sample != 16) {
            samples->clear(); Close(); return false;
        }
        remaining_ -= size;
        return true;
#else
        return false;
#endif
    }
    const auto bytes = std::min<size_t>(remaining_, 1024 * sizeof(int16_t));
    samples->resize(bytes / sizeof(int16_t));
    if (bytes != 0 && std::fread(samples->data(), 1, bytes, file_) != bytes) {
        samples->clear();
        Close();
        return false;
    }
    remaining_ -= bytes;
    return true;
}

void RecorderPlaybackStream::Close() {
#ifdef ESP_PLATFORM
    if (amr_decoder_) esp_amrnb_dec_close(amr_decoder_);
#endif
    amr_decoder_ = nullptr;
    amr_ = false;
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
    remaining_ = 0;
}

bool RecorderPlaybackAmrSelfTest() {
#ifdef ESP_PLATFORM
    // One synthetic silence frame; validate target decoder without playing a sound.
    uint8_t frame[] = {0x3c,0x91,0x17,0x16,0xbe,0x66,0x79,0xe1,0xe0,0x1,0xe7,0xaf,0xf0,0x0,0x0,0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};
    void* decoder = nullptr;
    if (esp_amrnb_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK) return false;
    int16_t pcm[160]{};
    esp_audio_dec_in_raw_t raw{}; raw.buffer = frame; raw.len = sizeof(frame);
    esp_audio_dec_out_frame_t output{}; output.buffer = reinterpret_cast<uint8_t*>(pcm); output.len = sizeof(pcm);
    esp_audio_dec_info_t info{};
    const bool ok = esp_amrnb_dec_decode(decoder, &raw, &output, &info) == ESP_AUDIO_ERR_OK &&
        raw.consumed == sizeof(frame) && output.decoded_size == sizeof(pcm) &&
        info.sample_rate == 8000 && info.bits_per_sample == 16 && info.channel == 1;
    esp_amrnb_dec_close(decoder);
    return ok;
#else
    return false;
#endif
}
