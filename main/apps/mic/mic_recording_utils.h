#ifndef MIC_RECORDING_UTILS_H_
#define MIC_RECORDING_UTILS_H_

#include <array>
#include <cstddef>
#include <cstdint>

constexpr int kMicMinGainPercent = 0;
constexpr int kMicMaxGainPercent = 100;
constexpr size_t kMicWavHeaderBytes = 44;

int MicClampGainPercent(int percent);
float MicInputGainDb(int percent);
std::array<uint8_t, kMicWavHeaderBytes> MicBuildStereoWavHeader(
    uint32_t data_bytes);

#endif  // MIC_RECORDING_UTILS_H_
