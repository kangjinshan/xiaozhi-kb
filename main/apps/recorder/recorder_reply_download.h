#ifndef RECORDER_REPLY_DOWNLOAD_H_
#define RECORDER_REPLY_DOWNLOAD_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#ifdef RECORDER_REPLY_DOWNLOAD_HOST_TEST
#include "recorder_reply_download_test_port.h"
#else
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#endif

// One background HTTPS GET with a bounded local receive buffer. Only the
// recorder main task reads this buffer and touches SD; no remote block ACKs.
class RecorderReplyDownload {
public:
    ~RecorderReplyDownload() { Close(); }
    bool Start(const std::string& url, uint64_t expected_bytes);
    int Read(uint8_t* destination, size_t capacity);
    bool active() const { return buffer_ != nullptr; }
    bool complete() const;
    void Close();

private:
    static void RunTask(void* context);
    void Run();
    StreamBufferHandle_t buffer_ = nullptr;
    std::string url_;
    uint64_t expected_bytes_ = 0;
    uint64_t delivered_bytes_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
};

#endif
