#include "recorder_wav_file.h"

#include <cstring>

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool RecorderParseWavHeader(const uint8_t* header, size_t len, RecorderWavInfo* info) {
    if (header == nullptr || info == nullptr || len < 44) {
        return false;
    }
    if (std::memcmp(header + 0, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0 ||
        std::memcmp(header + 12, "fmt ", 4) != 0 ||
        std::memcmp(header + 36, "data", 4) != 0) {
        return false;
    }

    const uint32_t fmt_size = ReadLe32(header + 16);
    const uint16_t audio_format = ReadLe16(header + 20);
    const uint16_t channels = ReadLe16(header + 22);
    const uint32_t sample_rate = ReadLe32(header + 24);
    const uint16_t block_align = ReadLe16(header + 32);
    const uint16_t bits_per_sample = ReadLe16(header + 34);
    const uint32_t data_bytes = ReadLe32(header + 40);

    if (fmt_size != 16 || audio_format != 1 || channels == 0 ||
        sample_rate == 0 || bits_per_sample != 16) {
        return false;
    }
    if (block_align != channels * (bits_per_sample / 8)) {
        return false;
    }

    info->sample_rate = sample_rate;
    info->channels = channels;
    info->bits_per_sample = bits_per_sample;
    info->data_offset = 44;
    info->data_bytes = data_bytes;
    return true;
}

bool RecorderWavMatchesCodec(const RecorderWavInfo& info, int sample_rate, int channels) {
    return info.sample_rate == static_cast<uint32_t>(sample_rate) &&
           info.channels == static_cast<uint16_t>(channels) &&
           info.bits_per_sample == 16 &&
           info.data_offset == 44 &&
           info.data_bytes > 0;
}
