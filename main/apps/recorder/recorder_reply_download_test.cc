#include "recorder_reply_download.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
struct TestStream { size_t capacity; std::mutex mutex; std::condition_variable changed; std::deque<uint8_t> bytes; };
struct TestHttp { size_t offset = 0; };
static std::vector<uint8_t> body(50000);
static std::atomic<bool> gate{true};
static std::atomic<int> requests{0};
static std::atomic<size_t> peak_buffer{0};
static int response_status = 200;
static int length_delta = 0;
static size_t truncate_at = 50000;
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t) { auto* s=new TestStream; s->capacity=size; return s; }
size_t xStreamBufferSend(TestStream* s, const void* data, size_t size, int wait) {
    std::unique_lock<std::mutex> lock(s->mutex);
    s->changed.wait_for(lock,std::chrono::milliseconds(wait),[&]{return s->bytes.size()<s->capacity;});
    size=std::min(size,s->capacity-s->bytes.size()); const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<size;++i)s->bytes.push_back(bytes[i]);
    peak_buffer=std::max(peak_buffer.load(),s->bytes.size()); return size;
}
size_t xStreamBufferReceive(TestStream* s, void* data, size_t size, int wait) {
    assert(wait==0); std::lock_guard<std::mutex> lock(s->mutex); size=std::min(size,s->bytes.size());
    auto* bytes=static_cast<uint8_t*>(data);
    for(size_t i=0;i<size;++i){bytes[i]=s->bytes.front();s->bytes.pop_front();}
    s->changed.notify_one(); return size;
}
void vStreamBufferDelete(TestStream* s){delete s;}
int xTaskCreate(void(*run)(void*),const char*,int stack,void* arg,int,void*) {
    assert(stack>=8192);std::thread([=]{run(arg);}).detach();return pdPASS;
}
void vTaskDelete(void*){}
void vTaskDelay(int ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
int esp_crt_bundle_attach(void*){return 0;}
TestHttp* esp_http_client_init(const esp_http_client_config_t* c){
    assert(c->crt_bundle_attach && c->disable_auto_redirect);return new TestHttp;
}
int esp_http_client_set_header(TestHttp*,const char*,const char*){return 0;}
int esp_http_client_open(TestHttp*,int){++requests;while(gate)std::this_thread::sleep_for(1ms);return 0;}
int64_t esp_http_client_fetch_headers(TestHttp*){return body.size()+length_delta;}
int esp_http_client_get_status_code(TestHttp*){return response_status;}
int esp_http_client_set_timeout_ms(TestHttp*,int){return 0;}
int esp_http_client_read(TestHttp* c,char* dst,int capacity){
    const size_t n=std::min<size_t>(capacity,truncate_at-c->offset);
    std::memcpy(dst,body.data()+c->offset,n);c->offset+=n;return n;
}
bool esp_http_client_is_complete_data_received(TestHttp* c){return c->offset==body.size();}
void esp_http_client_cleanup(TestHttp* c){delete c;}
int main(){
    for(size_t i=0;i<body.size();++i)body[i]=i%251;
    RecorderReplyDownload download;uint8_t data[4096];
    const auto start=std::chrono::steady_clock::now();
    assert(download.Start("https://agent.invalid/reply",body.size()));
    assert(std::chrono::steady_clock::now()-start<100ms);
    assert(download.Read(data,sizeof(data))==0); // A blocked TLS handshake cannot block microphone work.
    gate=false;std::this_thread::sleep_for(30ms);
    std::vector<uint8_t> actual;
    while(!download.complete()){
        int n=download.Read(data,sizeof(data));assert(n>=0);actual.insert(actual.end(),data,data+n);
        std::this_thread::sleep_for(1ms);
    }
    assert(actual==body && requests==1 && peak_buffer<=8192);
    download.Close();assert(!download.active());
    for(int error=0;error<3;++error){
        response_status=error==0?302:200;length_delta=error==1?1:0;truncate_at=error==2?9000:body.size();
        assert(download.Start("https://agent.invalid/reply",body.size()));
        bool failed=false;
        for(int i=0;i<1000 && !failed;++i){failed=download.Read(data,sizeof(data))<0;std::this_thread::sleep_for(1ms);}
        assert(failed && !download.complete());download.Close();
    }
    response_status=200;length_delta=0;truncate_at=body.size();
    assert(download.Start("https://agent.invalid/reply",body.size()));
    std::this_thread::sleep_for(30ms); // Producer is blocked by the full bounded receive buffer.
    auto closing=std::chrono::steady_clock::now();download.Close();
    assert(std::chrono::steady_clock::now()-closing<200ms);
    assert(!download.Start("http://agent.invalid/reply",body.size()));
}
