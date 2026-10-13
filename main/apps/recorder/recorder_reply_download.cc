#include "recorder_reply_download.h"
#include "agent_voice_protocol.h"
#include "system_info.h"
#include <esp_crt_bundle.h>
#include <algorithm>

bool RecorderReplyDownload::Start(const std::string& url, uint64_t expected_bytes) {
    Close();
    if (url.rfind("https://", 0) != 0 || expected_bytes < 44 ||
        expected_bytes > kAgentVoiceMaxBytes) return false;
    url_ = url;
    esp_http_client_config_t config{};
    config.url = url_.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;
    config.buffer_size = kAgentVoiceMaxChunkBytes;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = true;
    client_ = esp_http_client_init(&config);
    if (client_ == nullptr) return false;
    const std::string authorization = std::string("Bearer ") + CONFIG_AGENT_VOICE_TOKEN;
    esp_http_client_set_header(client_, "Authorization", authorization.c_str());
    esp_http_client_set_header(client_, "Device-Id", SystemInfo::GetMacAddress().c_str());
    esp_http_client_set_header(client_, "Protocol-Version", "1");
    esp_http_client_set_header(client_, "Accept", "audio/wav");
    const bool opened = esp_http_client_open(client_, 0) == ESP_OK;
    const int64_t length = opened ? esp_http_client_fetch_headers(client_) : -1;
    if (!opened || esp_http_client_get_status_code(client_) != 200 ||
        length != static_cast<int64_t>(expected_bytes)) {
        Close();
        return false;
    }
    expected_bytes_ = expected_bytes;
    received_bytes_ = 0;
    // Idle reads yield quickly so capture, dark touch and the WSS heartbeat run.
    esp_http_client_set_timeout_ms(client_, 40);
    return true;
}

int RecorderReplyDownload::Read(uint8_t* destination, size_t capacity) {
    if (!client_ || !destination || capacity == 0) return -1;
    if (received_bytes_ == expected_bytes_) return complete() ? 0 : -1;
    const auto wanted = std::min<uint64_t>(
        std::min<size_t>(capacity, kAgentVoiceMaxChunkBytes), expected_bytes_ - received_bytes_);
    const int count = esp_http_client_read(client_, reinterpret_cast<char*>(destination), wanted);
    if (count == -ESP_ERR_HTTP_EAGAIN) return 0;
    if (count <= 0) return -1;
    received_bytes_ += count;
    return count;
}

bool RecorderReplyDownload::complete() const {
    return client_ && received_bytes_ == expected_bytes_ &&
        esp_http_client_is_complete_data_received(client_);
}

void RecorderReplyDownload::Close() {
    if (client_) esp_http_client_cleanup(client_);
    client_ = nullptr;
    expected_bytes_ = received_bytes_ = 0;
    url_.clear();
}
