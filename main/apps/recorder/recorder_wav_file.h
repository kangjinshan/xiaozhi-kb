#ifndef RECORDER_WAV_FILE_H_
#define RECORDER_WAV_FILE_H_

#include <cstddef>
#include <cstdint>

struct RecorderWavInfo {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset = 0;
    uint32_t data_bytes = 0;
};

bool RecorderParseWavHeader(const uint8_t* header, size_t len, RecorderWavInfo* info);
bool RecorderWavMatchesCodec(const RecorderWavInfo& info, int sample_rate, int channels);

#endif  // RECORDER_WAV_FILE_H_
