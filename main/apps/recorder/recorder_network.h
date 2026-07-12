#ifndef RECORDER_NETWORK_H_
#define RECORDER_NETWORK_H_

#include <cstddef>
#include <cstdint>

class RecorderReconnectPolicy {
public:
    uint32_t NextDelayMs();
    uint32_t PeekBaseDelayMs() const { return base_delay_ms_; }
    void Reset();

private:
    uint32_t base_delay_ms_ = 1000;
    uint32_t attempt_ = 0;
};

#ifndef RECORDER_NETWORK_HOST_TEST

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_network.h>

class WebSocket;

enum class RecorderNetworkEventType {
    kWifiConnected,
    kWifiDisconnected,
    kSocketConnected,
    kSocketDisconnected,
    kText,
    kBinary,
    kNeedsWifiProvisioning,
    kNeedsAgentProvisioning,
    kError,
};

struct RecorderNetworkEvent {
    RecorderNetworkEventType type = RecorderNetworkEventType::kError;
    std::vector<uint8_t> data;

    std::string text() const {
        return std::string(data.begin(), data.end());
    }
};

class RecorderNetwork {
public:
    RecorderNetwork() = default;
    ~RecorderNetwork();

    bool Start();
    bool ConnectSocket();
    bool Poll(RecorderNetworkEvent* event);
    bool SendText(const std::string& text);
    bool SendBinary(const void* data, size_t size);
    void Ping();
    void CloseSocket();
    void Stop();
    bool IsSocketConnected() const;

private:
    static constexpr size_t kMaxQueuedEvents = 4;

    EspNetwork network_;
    std::unique_ptr<WebSocket> websocket_;
    mutable std::mutex queue_mutex_;
    std::deque<RecorderNetworkEvent> queue_;
    bool started_ = false;

    void Publish(RecorderNetworkEventType type,
                 const void* data = nullptr,
                 size_t size = 0);
};

#endif  // RECORDER_NETWORK_HOST_TEST

#endif  // RECORDER_NETWORK_H_
