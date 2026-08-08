#include "mic_wifi_audio.h"

#include "mic_wifi_protocol.h"

#include <array>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <ssid_manager.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wifi_manager.h>

#define TAG "mic_wifi"

namespace {

constexpr int64_t kSubscriberLeaseUs = 3500 * 1000;
constexpr int kSocketBufferBytes = 32 * 1024;

}  // namespace

MicWifiAudio::~MicWifiAudio() {
    Stop();
}

bool MicWifiAudio::Init() {
    if (started_) {
        return true;
    }
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        ESP_LOGW(TAG, "no saved Wi-Fi; configure Wi-Fi in XiaoZhi mode first");
        return false;
    }

    // WifiManager::Initialize() creates the esp-netif/lwIP tcpip mailbox.
    // Creating a BSD socket before this point asserts in tcpip_send_msg.
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = "zh-CN";
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.Initialize(config) && !wifi.IsInitialized()) {
        ESP_LOGE(TAG, "Wi-Fi manager initialization failed");
        return false;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_ < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno=%d", errno);
        return false;
    }
    int enabled = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
               &kSocketBufferBytes, sizeof(kSocketBufferBytes));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
               &kSocketBufferBytes, sizeof(kSocketBufferBytes));
    // The capture path has ~7 ms of deadline headroom. Let sendto wait for a
    // bounded 3 ms instead of dropping immediately when lwIP's queue is being
    // drained by the Wi-Fi task. Receive remains nonblocking via MSG_DONTWAIT.
    struct timeval send_timeout = {};
    send_timeout.tv_usec = 3000;
    if (setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
                   &send_timeout, sizeof(send_timeout)) < 0) {
        ESP_LOGE(TAG, "UDP send timeout setup failed: errno=%d", errno);
        close(socket_);
        socket_ = -1;
        return false;
    }

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kMicWifiPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ESP_LOGE(TAG, "UDP bind port %u failed: errno=%d",
                 static_cast<unsigned>(kMicWifiPort), errno);
        close(socket_);
        socket_ = -1;
        return false;
    }

    wifi.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Connected:
                connected_.store(true);
                WifiManager::GetInstance().SetPowerSaveLevel(
                    WifiPowerSaveLevel::PERFORMANCE);
                ESP_LOGI(TAG, "Wi-Fi connected: %s ip=%s UDP=%u",
                         data.c_str(),
                         WifiManager::GetInstance().GetIpAddress().c_str(),
                         static_cast<unsigned>(kMicWifiPort));
                break;
            case WifiEvent::Disconnected:
                connected_.store(false);
                ClearSubscriber();
                ESP_LOGW(TAG, "Wi-Fi disconnected");
                break;
            default:
                break;
        }
    });
    started_ = true;
    wifi.StartStation();
    ESP_LOGI(TAG, "real-time PCM receiver discovery listening on UDP %u",
             static_cast<unsigned>(kMicWifiPort));
    return true;
}

bool MicWifiAudio::SendPcm(const int16_t* stereo, size_t frames) {
    if (stereo == nullptr || frames != kMicWifiFramesPerPacket) {
        return false;
    }
    DrainListenRequests();

    struct sockaddr_in target = {};
    bool have_subscriber = false;
    const int64_t now_us = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex_);
        if (subscriber_expires_us_ > now_us) {
            target = subscriber_;
            have_subscriber = true;
        } else if (subscriber_expires_us_ != 0) {
            subscriber_expires_us_ = 0;
        }
    }
    streaming_.store(connected_.load() && have_subscriber);

    const uint32_t packet_timestamp = timestamp_frames_;
    timestamp_frames_ += static_cast<uint32_t>(frames);
    if (!streaming_.load() || socket_ < 0) {
        return false;
    }

    std::array<uint8_t, kMicWifiAudioPacketBytes> packet = {};
    MicWifiWriteAudioHeader(packet.data(), sequence_, packet_timestamp);
    std::memcpy(packet.data() + kMicWifiAudioHeaderBytes,
                stereo, kMicWifiPcmBytes);
    const ssize_t sent = sendto(
        socket_, packet.data(), packet.size(), 0,
        reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    ++sequence_;
    if (sent != static_cast<ssize_t>(packet.size())) {
        dropped_packets_.fetch_add(1);
        return false;
    }
    sent_packets_.fetch_add(1);
    return true;
}

void MicWifiAudio::DrainListenRequests() {
    if (socket_ < 0) {
        return;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint8_t request[64];
        struct sockaddr_in source = {};
        socklen_t source_size = sizeof(source);
        const ssize_t received = recvfrom(
            socket_, request, sizeof(request), MSG_DONTWAIT,
            reinterpret_cast<sockaddr*>(&source), &source_size);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "UDP receive failed: errno=%d", errno);
            }
            break;
        }
        if (!MicWifiIsListenRequest(
                request, static_cast<size_t>(received)) ||
            source.sin_family != AF_INET || source.sin_port == 0) {
            continue;
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(subscriber_mutex_);
            changed = subscriber_expires_us_ == 0 ||
                      subscriber_.sin_addr.s_addr != source.sin_addr.s_addr ||
                      subscriber_.sin_port != source.sin_port;
            subscriber_ = source;
            subscriber_expires_us_ =
                esp_timer_get_time() + kSubscriberLeaseUs;
        }
        streaming_.store(connected_.load());
        if (changed) {
            char address[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address));
            ESP_LOGI(TAG, "real-time receiver %s:%u subscribed",
                     address, static_cast<unsigned>(ntohs(source.sin_port)));
        }
    }
}

void MicWifiAudio::ClearSubscriber() {
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    subscriber_ = {};
    subscriber_expires_us_ = 0;
    streaming_.store(false);
}

void MicWifiAudio::Stop() {
    if (started_) {
        auto& wifi = WifiManager::GetInstance();
        wifi.SetEventCallback({});
        wifi.StopStation();
        started_ = false;
    }
    connected_.store(false);
    ClearSubscriber();
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}
