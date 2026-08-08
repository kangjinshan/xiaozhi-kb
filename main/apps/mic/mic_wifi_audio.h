#ifndef MIC_WIFI_AUDIO_H_
#define MIC_WIFI_AUDIO_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <lwip/sockets.h>

// Low-latency LAN transport for the ESP32-C6 microphone. A desktop receiver
// sends a discovery heartbeat, then the device unicasts one PCM16 stereo UDP
// datagram every 20 ms until that receiver's short lease expires.
class MicWifiAudio {
public:
    MicWifiAudio() = default;
    ~MicWifiAudio();

    MicWifiAudio(const MicWifiAudio&) = delete;
    MicWifiAudio& operator=(const MicWifiAudio&) = delete;

    bool Init();
    bool SendPcm(const int16_t* interleaved_stereo, size_t frames);
    void Stop();

    bool connected() const { return connected_.load(); }
    bool streaming() const { return streaming_.load(); }
    uint32_t sent_packets() const { return sent_packets_.load(); }
    uint32_t dropped_packets() const { return dropped_packets_.load(); }

private:
    void DrainListenRequests();
    void ClearSubscriber();

    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> sent_packets_{0};
    std::atomic<uint32_t> dropped_packets_{0};
    std::mutex subscriber_mutex_;
    struct sockaddr_in subscriber_ = {};
    int64_t subscriber_expires_us_ = 0;
    uint32_t sequence_ = 0;
    uint32_t timestamp_frames_ = 0;
    int socket_ = -1;
    bool started_ = false;
};

#endif  // MIC_WIFI_AUDIO_H_
