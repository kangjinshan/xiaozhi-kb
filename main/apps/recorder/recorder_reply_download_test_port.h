#ifndef RECORDER_REPLY_DOWNLOAD_TEST_PORT_H_
#define RECORDER_REPLY_DOWNLOAD_TEST_PORT_H_
#include <cstddef>
#include <cstdint>
#include <string>
struct TestStream;
struct TestHttp;
using StreamBufferHandle_t = TestStream*;
constexpr int pdPASS = 1;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_HTTP_EAGAIN = 0x7007;
#define pdMS_TO_TICKS(value) (value)
#define CONFIG_AGENT_VOICE_TOKEN "unit-test-device-token"
StreamBufferHandle_t xStreamBufferCreate(size_t, size_t);
size_t xStreamBufferSend(StreamBufferHandle_t, const void*, size_t, int);
size_t xStreamBufferReceive(StreamBufferHandle_t, void*, size_t, int);
void vStreamBufferDelete(StreamBufferHandle_t);
int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*);
void vTaskDelete(void*);
void vTaskDelay(int);
struct esp_http_client_config_t {
    const char* url = nullptr;
    int (*crt_bundle_attach)(void*) = nullptr;
    int timeout_ms = 0, buffer_size = 0, buffer_size_tx = 0;
    bool disable_auto_redirect = false;
};
int esp_crt_bundle_attach(void*);
TestHttp* esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_set_header(TestHttp*, const char*, const char*);
int esp_http_client_open(TestHttp*, int);
int64_t esp_http_client_fetch_headers(TestHttp*);
int esp_http_client_get_status_code(TestHttp*);
int esp_http_client_set_timeout_ms(TestHttp*, int);
int esp_http_client_read(TestHttp*, char*, int);
bool esp_http_client_is_complete_data_received(TestHttp*);
void esp_http_client_cleanup(TestHttp*);
struct SystemInfo { static std::string GetMacAddress() { return "test-device"; } };
#endif
