#include "mic_recording_utils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint16_t ReadLe16(const uint8_t* source) {
    return static_cast<uint16_t>(source[0]) |
           static_cast<uint16_t>(source[1]) << 8;
}

uint32_t ReadLe32(const uint8_t* source) {
    return static_cast<uint32_t>(source[0]) |
           static_cast<uint32_t>(source[1]) << 8 |
           static_cast<uint32_t>(source[2]) << 16 |
           static_cast<uint32_t>(source[3]) << 24;
}

}  // namespace

int main() {
    Check(MicClampGainPercent(-10) == 0, "gain lower clamp");
    Check(MicClampGainPercent(110) == 100, "gain upper clamp");
    Check(std::fabs(MicInputGainDb(0) - 12.0f) < 0.001f,
          "zero percent maps to 12 dB");
    Check(std::fabs(MicInputGainDb(100) - 37.5f) < 0.001f,
          "full percent maps to 37.5 dB");

    const auto header = MicBuildStereoWavHeader(1280);
    Check(std::memcmp(header.data(), "RIFF", 4) == 0, "RIFF signature");
    Check(ReadLe32(header.data() + 4) == 1316, "RIFF size");
    Check(std::memcmp(header.data() + 8, "WAVEfmt ", 8) == 0,
          "WAVE fmt signature");
    Check(ReadLe16(header.data() + 20) == 1, "PCM format");
    Check(ReadLe16(header.data() + 22) == 2, "stereo channels");
    Check(ReadLe32(header.data() + 24) == 16000, "sample rate");
    Check(ReadLe32(header.data() + 28) == 64000, "byte rate");
    Check(ReadLe16(header.data() + 32) == 4, "block alignment");
    Check(ReadLe16(header.data() + 34) == 16, "bits per sample");
    Check(std::memcmp(header.data() + 36, "data", 4) == 0,
          "data signature");
    Check(ReadLe32(header.data() + 40) == 1280, "data size");
    return 0;
}
