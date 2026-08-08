#include "mic_recording_utils.h"

#include <algorithm>
#include <cstring>

namespace {

void PutLe16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xFF);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void PutLe32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value & 0xFF);
    destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    destination[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

int MicClampGainPercent(int percent) {
    return std::clamp(percent, kMicMinGainPercent, kMicMaxGainPercent);
}

float MicInputGainDb(int percent) {
    const int clamped = MicClampGainPercent(percent);
    return 12.0f + 25.5f * static_cast<float>(clamped) / 100.0f;
}

std::array<uint8_t, kMicWavHeaderBytes> MicBuildStereoWavHeader(
    uint32_t data_bytes) {
    constexpr uint32_t kSampleRate = 16000;
    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kChannels * sizeof(int16_t);
    constexpr uint32_t kByteRate = kSampleRate * kBlockAlign;

    std::array<uint8_t, kMicWavHeaderBytes> header = {};
    std::memcpy(header.data(), "RIFF", 4);
    PutLe32(header.data() + 4, 36 + data_bytes);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    PutLe32(header.data() + 16, 16);
    PutLe16(header.data() + 20, 1);
    PutLe16(header.data() + 22, kChannels);
    PutLe32(header.data() + 24, kSampleRate);
    PutLe32(header.data() + 28, kByteRate);
    PutLe16(header.data() + 32, kBlockAlign);
    PutLe16(header.data() + 34, kBitsPerSample);
    std::memcpy(header.data() + 36, "data", 4);
    PutLe32(header.data() + 40, data_bytes);
    return header;
}
