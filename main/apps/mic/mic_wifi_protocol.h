#ifndef MIC_WIFI_PROTOCOL_H_
#define MIC_WIFI_PROTOCOL_H_

#include <array>
#include <cstddef>
#include <cstdint>

constexpr uint16_t kMicWifiPort = 45830;
constexpr uint8_t kMicWifiProtocolVersion = 1;
constexpr size_t kMicWifiControlBytes = 8;
constexpr size_t kMicWifiAudioHeaderBytes = 24;
constexpr size_t kMicWifiFramesPerPacket = 320;
constexpr size_t kMicWifiChannels = 2;
constexpr size_t kMicWifiPcmBytes =
    kMicWifiFramesPerPacket * kMicWifiChannels * sizeof(int16_t);
constexpr size_t kMicWifiAudioPacketBytes =
    kMicWifiAudioHeaderBytes + kMicWifiPcmBytes;

// A receiver broadcasts this small heartbeat once per second. The device
// streams to the heartbeat's source address for a short lease, so microphone
// audio is not broadcast continuously to every host on the LAN.
std::array<uint8_t, kMicWifiControlBytes> MicWifiBuildListenRequest();
bool MicWifiIsListenRequest(const uint8_t* data, size_t size);

// Writes the fixed little-endian header used by each 20 ms PCM16 stereo UDP
// datagram. The 1280-byte PCM payload follows immediately after this header.
void MicWifiWriteAudioHeader(uint8_t* output,
                             uint32_t sequence,
                             uint32_t timestamp_frames);

#endif  // MIC_WIFI_PROTOCOL_H_
