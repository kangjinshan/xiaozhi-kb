#include "recorder_reply_download.h"
#include "agent_voice_protocol.h"
#ifndef RECORDER_REPLY_DOWNLOAD_HOST_TEST
#include "system_info.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/task.h>
#endif
#include <algorithm>
#include <vector>

bool RecorderReplyDownload::Start(const std::string& url, uint64_t expected_bytes) {
    Close();
    if (url.rfind("https://", 0) != 0 || expected_bytes < 7 ||
        expected_bytes > kAgentVoiceMaxBytes) return false;
    url_ = url;
    expected_bytes_ = expected_bytes;
    delivered_bytes_ = 0;
    stop_ = failed_ = false;
    buffer_ = xStreamBufferCreate(8192, 1);
    if (!buffer_) return false;
    running_ = true;
    if (xTaskCreate(RunTask, "reply_http", 12288, this, 1, nullptr) != pdPASS) {
        running_ = false;
        Close();
        return false;
    }
    return true;
}

void RecorderReplyDownload::RunTask(void* context) {
    auto* download = static_cast<RecorderReplyDownload*>(context);
    download->Run();
    download->running_ = false;
    vTaskDelete(nullptr);
}

void RecorderReplyDownload::Run() {
    esp_http_client_config_t config{};
    config.url = url_.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 5000;
    config.buffer_size = kAgentVoiceMaxChunkBytes;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = true;
    auto* client = esp_http_client_init(&config);
    if (!client) { failed_ = true; return; }
    const std::string authorization = std::string("Bearer ") + CONFIG_AGENT_VOICE_TOKEN;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Device-Id", SystemInfo::GetMacAddress().c_str());
    esp_http_client_set_header(client, "Protocol-Version", "1");
    esp_http_client_set_header(client, "Accept", "audio/amr, audio/wav");
    bool ok = !stop_ && esp_http_client_open(client, 0) == ESP_OK;
    const int64_t length = ok ? esp_http_client_fetch_headers(client) : -1;
    ok = ok && esp_http_client_get_status_code(client) == 200 &&
        length == static_cast<int64_t>(expected_bytes_);
    esp_http_client_set_timeout_ms(client, 200);
    std::vector<uint8_t> block(kAgentVoiceMaxChunkBytes);
    uint64_t received = 0;
    while (ok && !stop_ && received < expected_bytes_) {
        const auto wanted = std::min<uint64_t>(block.size(), expected_bytes_ - received);
        const int count = esp_http_client_read(client, reinterpret_cast<char*>(block.data()), wanted);
        if (count == -ESP_ERR_HTTP_EAGAIN) continue;
        if (count <= 0) { ok = false; break; }
        size_t offset = 0;
        while (!stop_ && offset < static_cast<size_t>(count)) {
            offset += xStreamBufferSend(buffer_, block.data() + offset,
                                         count - offset, pdMS_TO_TICKS(20));
        }
        received += count;
    }
    ok = ok && received == expected_bytes_ && esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    failed_ = !ok && !stop_;
}

int RecorderReplyDownload::Read(uint8_t* destination, size_t capacity) {
    if (!buffer_ || !destination || !capacity || failed_) return -1;
    const auto count = xStreamBufferReceive(buffer_, destination,
        std::min<size_t>(capacity, kAgentVoiceMaxChunkBytes), 0);
    delivered_bytes_ += count;
    return static_cast<int>(count);
}

bool RecorderReplyDownload::complete() const {
    return buffer_ && !running_ && !failed_ && delivered_bytes_ == expected_bytes_;
}

void RecorderReplyDownload::Close() {
    stop_ = true;
    while (running_) vTaskDelay(1);
    if (buffer_) vStreamBufferDelete(buffer_);
    buffer_ = nullptr;
    expected_bytes_ = delivered_bytes_ = 0;
    url_.clear();
}
