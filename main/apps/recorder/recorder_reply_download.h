#ifndef RECORDER_REPLY_DOWNLOAD_H_
#define RECORDER_REPLY_DOWNLOAD_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <esp_http_client.h>

// A single HTTPS GET. Reads use a bounded buffer while TCP handles backpressure;
// there is no application-level request or acknowledgement for individual blocks.
class RecorderReplyDownload {
public:
    ~RecorderReplyDownload() { Close(); }
    bool Start(const std::string& url, uint64_t expected_bytes);
    int Read(uint8_t* destination, size_t capacity);
    bool active() const { return client_ != nullptr; }
    bool complete() const;
    void Close();

private:
    esp_http_client_handle_t client_ = nullptr;
    std::string url_;
    uint64_t expected_bytes_ = 0;
    uint64_t received_bytes_ = 0;
};

#endif
