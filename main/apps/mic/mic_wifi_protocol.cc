#include "mic_wifi_protocol.h"

#include <algorithm>

namespace {

constexpr uint8_t kMagic[] = {'X', 'M', 'I', 'C'};
constexpr uint8_t kListenType = 1;
constexpr uint8_t kAudioType = 2;

void WriteLe16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLe32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8);
    output[2] = static_cast<uint8_t>(value >> 16);
    output[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace

std::array<uint8_t, kMicWifiControlBytes> MicWifiBuildListenRequest() {
    std::array<uint8_t, kMicWifiControlBytes> request = {};
    std::copy(std::begin(kMagic), std::end(kMagic), request.begin());
    request[4] = kMicWifiProtocolVersion;
    request[5] = kListenType;
    return request;
}

bool MicWifiIsListenRequest(const uint8_t* data, size_t size) {
    return data != nullptr && size == kMicWifiControlBytes &&
           std::equal(std::begin(kMagic), std::end(kMagic), data) &&
           data[4] == kMicWifiProtocolVersion && data[5] == kListenType &&
           data[6] == 0 && data[7] == 0;
}

void MicWifiWriteAudioHeader(uint8_t* output,
                             uint32_t sequence,
                             uint32_t timestamp_frames) {
    if (output == nullptr) {
        return;
    }
    std::copy(std::begin(kMagic), std::end(kMagic), output);
    output[4] = kMicWifiProtocolVersion;
    output[5] = kAudioType;
    WriteLe16(output + 6, kMicWifiAudioHeaderBytes);
    WriteLe32(output + 8, sequence);
    WriteLe32(output + 12, timestamp_frames);
    WriteLe32(output + 16, 16000);
    WriteLe16(output + 20, kMicWifiFramesPerPacket);
    output[22] = kMicWifiChannels;
    output[23] = 16;
}
