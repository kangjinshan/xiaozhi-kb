#include "recorder_network.h"

#include <algorithm>

uint32_t RecorderReconnectPolicy::NextDelayMs() {
    const uint32_t jitter = (attempt_ % 5) * (base_delay_ms_ / 40);
    const uint32_t delay = base_delay_ms_ + jitter;
    base_delay_ms_ = std::min<uint32_t>(base_delay_ms_ * 2, 30000);
    ++attempt_;
    return delay;
}

void RecorderReconnectPolicy::Reset() {
    base_delay_ms_ = 1000;
    attempt_ = 0;
}

#ifndef RECORDER_NETWORK_HOST_TEST

#include "system_info.h"

#include <esp_log.h>
#include <ssid_manager.h>
#include <web_socket.h>
#include <wifi_manager.h>

#define TAG "recorder_network"

RecorderNetwork::~RecorderNetwork() {
    Stop();
}

bool RecorderNetwork::Start() {
    if (started_) {
        return true;
    }
    if (std::string(CONFIG_AGENT_VOICE_TOKEN).empty()) {
        Publish(RecorderNetworkEventType::kNeedsAgentProvisioning);
        return false;
    }
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        Publish(RecorderNetworkEventType::kNeedsWifiProvisioning);
        return false;
    }
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = "zh-CN";
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.Initialize(config) && !wifi.IsInitialized()) {
        Publish(RecorderNetworkEventType::kError);
        return false;
    }
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        switch (event) {
            case WifiEvent::Connected:
                Publish(RecorderNetworkEventType::kWifiConnected);
                break;
            case WifiEvent::Disconnected:
                Publish(RecorderNetworkEventType::kWifiDisconnected);
                break;
            default:
                break;
        }
    });
    started_ = true;
    wifi.StartStation();
    return true;
}

bool RecorderNetwork::ConnectSocket() {
    CloseSocket();
    websocket_ = network_.CreateWebSocket(2);
    if (websocket_ == nullptr) {
        Publish(RecorderNetworkEventType::kError);
        return false;
    }
    std::string authorization = "Bearer ";
    authorization += CONFIG_AGENT_VOICE_TOKEN;
    websocket_->SetHeader("Authorization", authorization.c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Protocol-Version", "1");
    websocket_->SetReceiveBufferSize(8192);
    websocket_->OnConnected([this]() {
        Publish(RecorderNetworkEventType::kSocketConnected);
    });
    websocket_->OnDisconnected([this]() {
        Publish(RecorderNetworkEventType::kSocketDisconnected);
    });
    websocket_->OnError([this](int error) {
        Publish(RecorderNetworkEventType::kError, &error, sizeof(error));
    });
    websocket_->OnData([this](const char* data, size_t size, bool binary) {
        Publish(
            binary ? RecorderNetworkEventType::kBinary : RecorderNetworkEventType::kText,
            data,
            size);
    });
    ESP_LOGI(TAG, "Connecting Agent voice WebSocket");
    if (!websocket_->Connect(CONFIG_AGENT_VOICE_URL)) {
        ESP_LOGE(TAG, "Agent voice WebSocket connect failed: %d", websocket_->GetLastError());
        websocket_.reset();
        Publish(RecorderNetworkEventType::kError);
        return false;
    }
    return true;
}

bool RecorderNetwork::Poll(RecorderNetworkEvent* event) {
    if (event == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) {
        return false;
    }
    *event = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool RecorderNetwork::SendText(const std::string& text) {
    return websocket_ != nullptr && websocket_->IsConnected() && websocket_->Send(text);
}

bool RecorderNetwork::SendBinary(const void* data, size_t size) {
    return websocket_ != nullptr && websocket_->IsConnected() &&
           data != nullptr && size > 0 && size <= 4096 &&
           websocket_->Send(data, size, true);
}

void RecorderNetwork::Ping() {
    if (websocket_ != nullptr && websocket_->IsConnected()) {
        websocket_->Ping();
    }
}

void RecorderNetwork::CloseSocket() {
    if (websocket_ != nullptr) {
        websocket_->Close();
        websocket_.reset();
    }
}

void RecorderNetwork::Stop() {
    CloseSocket();
    if (started_) {
        auto& wifi = WifiManager::GetInstance();
        wifi.SetEventCallback({});
        wifi.StopStation();
        started_ = false;
    }
}

bool RecorderNetwork::IsSocketConnected() const {
    return websocket_ != nullptr && websocket_->IsConnected();
}

void RecorderNetwork::Publish(RecorderNetworkEventType type,
                              const void* data,
                              size_t size) {
    RecorderNetworkEvent event;
    event.type = type;
    if (data != nullptr && size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        event.data.assign(bytes, bytes + size);
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= kMaxQueuedEvents) {
        queue_.clear();
        RecorderNetworkEvent overflow;
        overflow.type = RecorderNetworkEventType::kError;
        const char message[] = "network event queue overflow";
        overflow.data.assign(message, message + sizeof(message) - 1);
        queue_.push_back(std::move(overflow));
        return;
    }
    queue_.push_back(std::move(event));
}

#endif  // RECORDER_NETWORK_HOST_TEST
