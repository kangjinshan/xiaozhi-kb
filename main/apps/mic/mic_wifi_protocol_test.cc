#include "mic_wifi_protocol.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint16_t ReadLe16(const uint8_t* input) {
    return static_cast<uint16_t>(input[0]) |
           (static_cast<uint16_t>(input[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* input) {
    return static_cast<uint32_t>(input[0]) |
           (static_cast<uint32_t>(input[1]) << 8) |
           (static_cast<uint32_t>(input[2]) << 16) |
           (static_cast<uint32_t>(input[3]) << 24);
}

}  // namespace

int main() {
    const auto request = MicWifiBuildListenRequest();
    Check(MicWifiIsListenRequest(request.data(), request.size()),
          "listen request round trip");
    auto bad_request = request;
    bad_request[4]++;
    Check(!MicWifiIsListenRequest(bad_request.data(), bad_request.size()),
          "wrong protocol version rejected");
    Check(!MicWifiIsListenRequest(request.data(), request.size() - 1),
          "wrong control length rejected");

    uint8_t header[kMicWifiAudioHeaderBytes] = {};
    MicWifiWriteAudioHeader(header, 0x78563412, 0xF0DEBC9A);
    Check(header[0] == 'X' && header[1] == 'M' &&
              header[2] == 'I' && header[3] == 'C',
          "audio magic");
    Check(header[4] == 1 && header[5] == 2, "audio version and type");
    Check(ReadLe16(header + 6) == kMicWifiAudioHeaderBytes,
          "audio header size");
    Check(ReadLe32(header + 8) == 0x78563412, "sequence little endian");
    Check(ReadLe32(header + 12) == 0xF0DEBC9A,
          "timestamp little endian");
    Check(ReadLe32(header + 16) == 16000, "sample rate");
    Check(ReadLe16(header + 20) == kMicWifiFramesPerPacket,
          "frame count");
    Check(header[22] == 2 && header[23] == 16, "stereo PCM16");
    Check(kMicWifiAudioPacketBytes == 1304,
          "audio datagram remains below Ethernet MTU");
    return 0;
}
