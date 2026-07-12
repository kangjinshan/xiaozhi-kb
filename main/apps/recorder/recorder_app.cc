#include "recorder_app.h"
#include "recorder_display.h"

#include "agent_turn_store.h"
#include "agent_voice_protocol.h"
#include "agent_voice_state.h"
#include "config.h"
#include "sdcard.h"
#include "button.h"
#include "app_mode.h"
#include "codecs/box_audio_codec.h"
#include "recorder_file_list.h"
#include "recorder_control_state.h"
#include "recorder_noise_reducer.h"
#include "recorder_network.h"
#include "recorder_rate_converter.h"
#include "recorder_turn_clock.h"
#include "recorder_wav_file.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <mbedtls/sha256.h>

#define TAG "recorder_app"

namespace {

constexpr uint8_t kAxp2101Address = 0x34;
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
const char* kRecDir = "/sdcard/rec";
const char* kAgentRoot = "/sdcard/agent";
constexpr int kFrameSamples = 1024;  // 每次读取的采样点数（单声道）
constexpr size_t kPlaybackReadSamples = 4096;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_pmic = nullptr;

struct RecorderRequest {
    RecorderControlEvent event;
    std::string path;
};

std::mutex s_request_mutex;
std::deque<RecorderRequest> s_requests;

esp_err_t WriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

// 建 I2C 总线（I2C_NUM_0, SDA=GPIO8/SCL=GPIO7）+ AXP2101 电源管理，
// 与键盘模式 InitializeTouch/InitializeKeyboardPmic 一致，保证屏与麦克风都有供电。
esp_err_t InitI2cAndPmic() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = kI2cPort;
    bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAxp2101Address;
    dev_cfg.scl_speed_hz = 400 * 1000;
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pmic);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 添加失败: %s", esp_err_to_name(err));
        return err;
    }

    // AXP2101 供电寄存器序列（与键盘模式 InitializeKeyboardPmic 一致）
    const struct { uint8_t reg; uint8_t val; } writes[] = {
        {0x22, 0b00000110}, {0x27, 0x10}, {0x80, 0x01}, {0x90, 0x00}, {0x91, 0x00},
        {0x82, static_cast<uint8_t>((3300 - 1500) / 100)},
        {0x92, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x93, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x94, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x95, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x90, 0x0F}, {0x64, 0x02}, {0x61, 0x02}, {0x62, 0x0A}, {0x63, 0x01},
    };
    for (const auto& w : writes) {
        err = WriteReg(s_pmic, w.reg, w.val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 写寄存器 0x%02x 失败: %s", w.reg, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

// 写 44 字节 WAV 头。data_bytes=0 时为占位（录音结束再回填）。
void WriteWavHeader(FILE* f, uint32_t sample_rate, uint16_t channels,
                    uint16_t bits, uint32_t data_bytes) {
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);
    uint32_t riff_size = 36 + data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16; fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1;  fwrite(&audio_fmt, 2, 1, f);   // PCM
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

bool AppendPcm(FILE* file, const std::vector<int16_t>& samples, uint32_t* data_bytes) {
    if (file == nullptr || data_bytes == nullptr) {
        return false;
    }
    const size_t bytes = samples.size() * sizeof(int16_t);
    if (bytes == 0) {
        return true;
    }
    if (fwrite(samples.data(), 1, bytes, file) != bytes) {
        return false;
    }
    *data_bytes += static_cast<uint32_t>(bytes);
    return true;
}

bool FinishRecording(FILE* file,
                     RecorderNoiseReducer* reducer,
                     uint32_t* data_bytes) {
    std::vector<int16_t> tail;
    if (!reducer->Flush(&tail) || !AppendPcm(file, tail, data_bytes)) {
        return false;
    }
    WriteWavHeader(file, reducer->output_sample_rate(), 1, 16, *data_bytes);
    return std::fflush(file) == 0 && fsync(fileno(file)) == 0;
}

uint64_t RecorderMonotonicMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

std::string DigestHex(const unsigned char digest[32]) {
    static const char kHex[] = "0123456789abcdef";
    std::string output(64, '0');
    for (size_t index = 0; index < 32; ++index) {
        output[index * 2] = kHex[digest[index] >> 4];
        output[index * 2 + 1] = kHex[digest[index] & 0x0F];
    }
    return output;
}

bool HashSdFile(const std::string& path,
                uint64_t* file_bytes,
                std::string* sha256) {
    if (file_bytes == nullptr || sha256 == nullptr) {
        return false;
    }
    RecorderDisplayPause();
    FILE* file = fopen(path.c_str(), "rb");
    RecorderDisplayResume();
    if (file == nullptr) {
        return false;
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    uint64_t total = 0;
    std::vector<unsigned char> buffer(kAgentVoiceMaxChunkBytes);
    while (ok) {
        RecorderDisplayPause();
        const size_t size = fread(buffer.data(), 1, buffer.size(), file);
        const bool read_failed = ferror(file) != 0;
        RecorderDisplayResume();
        if (size > 0) {
            ok = mbedtls_sha256_update(&context, buffer.data(), size) == 0;
            total += size;
        }
        if (size < buffer.size()) {
            ok = ok && !read_failed;
            break;
        }
    }
    RecorderDisplayPause();
    fclose(file);
    RecorderDisplayResume();
    unsigned char digest[32];
    ok = ok && mbedtls_sha256_finish(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!ok) {
        return false;
    }
    *file_bytes = total;
    *sha256 = DigestHex(digest);
    return true;
}

void PublishRequest(RecorderControlEvent event, const char* path = nullptr) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    s_requests.push_back({event, path != nullptr ? path : ""});
}

bool TakeRequest(RecorderRequest* request) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    if (request == nullptr || s_requests.empty()) {
        return false;
    }
    *request = std::move(s_requests.front());
    s_requests.pop_front();
    return true;
}

void OnRecordRequested(void*) {
    PublishRequest(RecorderControlEvent::kTouchRecord);
}

void OnStopRequested(void*) {
    PublishRequest(RecorderControlEvent::kTouchStop);
}

void OnOpenPlaybackMenu(void*) {
    PublishRequest(RecorderControlEvent::kTouchPlay);
}

void OnPauseResumeRequested(void*) {
    PublishRequest(RecorderControlEvent::kTouchPauseResume);
}

void OnExitRequested(void*) {
    PublishRequest(RecorderControlEvent::kExitRequested);
}

void OnPlayFileRequested(const char* path, void*) {
    if (path != nullptr) {
        PublishRequest(RecorderControlEvent::kPlaybackSelected, path);
    }
}

struct RecorderAssistantRenderContext {
    RecorderControlState* control = nullptr;
    AgentVoiceState* voice_state = nullptr;
    RecorderAssistantNotice* notice = nullptr;
    uint32_t* elapsed_seconds = nullptr;
    bool sd_ready = false;
    bool noise_reduction_ready = false;
};

void RenderAssistant(const RecorderAssistantRenderContext* context) {
    if (context == nullptr || context->control == nullptr ||
        context->voice_state == nullptr || context->notice == nullptr ||
        context->elapsed_seconds == nullptr) {
        return;
    }
    RecorderAssistantUiInput input;
    input.mode = context->control->mode;
    input.voice_phase = context->voice_state->phase;
    input.turn_pending = context->voice_state->queued_turn;
    input.sd_ready = context->sd_ready;
    input.noise_reduction_ready = context->noise_reduction_ready;
    input.notice = *context->notice;
    input.elapsed_seconds = *context->elapsed_seconds;
    input.volume = context->control->volume;
    RecorderDisplayRenderAssistant(RecorderBuildAssistantUi(input));
}

RecorderControlAction ApplyRequest(const RecorderRequest& request,
                                   RecorderControlState* control,
                                   BoxAudioCodec* codec,
                                   bool* exit_requested,
                                   RecorderAssistantRenderContext* ui_context) {
    RecorderControlAction action = RecorderControlReduce(control, request.event);
    if (action == RecorderControlAction::kVolumeChanged && codec != nullptr) {
        codec->SetOutputVolume(control->volume);
    } else if (action == RecorderControlAction::kExit && exit_requested != nullptr) {
        *exit_requested = true;
    }
    if (action == RecorderControlAction::kVolumeChanged ||
        action == RecorderControlAction::kPausePlayback ||
        action == RecorderControlAction::kResumePlayback) {
        RenderAssistant(ui_context);
    }
    return action;
}

void DrainPlaybackRequests(RecorderControlState* control,
                           BoxAudioCodec* codec,
                           bool* exit_requested,
                           RecorderAssistantRenderContext* ui_context) {
    RecorderRequest request;
    while (TakeRequest(&request)) {
        ApplyRequest(request, control, codec, exit_requested, ui_context);
    }
}

void FlushDisplayThenPause(uint32_t ms = 90) {
    RecorderDisplayResume();
    vTaskDelay(pdMS_TO_TICKS(ms));
    RecorderDisplayPause();
}

void ShowPlaybackMenu() {
    std::vector<RecorderDisplayMenuItem> menu_items;
    auto entries = RecorderListAgentRecordings(kAgentRoot, 32);
    auto legacy = RecorderListRecordings(kRecDir, 32);
    entries.insert(entries.end(), legacy.begin(), legacy.end());
    if (entries.size() > 32) {
        entries.resize(32);
    }
    menu_items.reserve(entries.size());
    for (const auto& entry : entries) {
        RecorderDisplayMenuItem item;
        item.label = RecorderConversationLabel(entry);
        item.detail = RecorderFormatRecordingDetail(entry);
        item.path = entry.path;
        item.conversation_detail = !entry.conversation_text.empty();
        menu_items.push_back(item);
    }
    RecorderDisplayShowFileMenu(menu_items);
}

enum class PlaybackResult {
    kDone,
    kFailed,
    kInterrupted,
};

// Caller enters with LVGL paused. Every return path leaves exactly one pause
// held so file close and the caller's result-screen update stay SPI2-safe.
PlaybackResult PlayWavFile(BoxAudioCodec& codec,
                           const char* path,
                           RecorderControlState* control,
                           bool* exit_requested,
                           RecorderAssistantRenderContext* ui_context) {
    FILE* playback = fopen(path, "rb");
    if (playback == nullptr) {
        ESP_LOGE(TAG, "无法打开播放文件 %s", path);
        return PlaybackResult::kFailed;
    }

    uint8_t header[44];
    const size_t header_bytes = fread(header, 1, sizeof(header), playback);
    RecorderWavInfo info = {};
    if (header_bytes != sizeof(header) ||
        !RecorderParseWavHeader(header, sizeof(header), &info) ||
        !RecorderWavCanPlay(info, codec.output_sample_rate(), codec.output_channels())) {
        ESP_LOGE(TAG, "不支持播放的 WAV: %s", path);
        fclose(playback);
        return PlaybackResult::kFailed;
    }

    RecorderRateConverter playback_rate(info.sample_rate, codec.output_sample_rate());
    if (!playback_rate.valid()) {
        ESP_LOGE(TAG, "播放重采样器初始化失败: %u -> %d",
                 static_cast<unsigned>(info.sample_rate),
                 codec.output_sample_rate());
        fclose(playback);
        return PlaybackResult::kFailed;
    }

    if (fseek(playback, info.data_offset, SEEK_SET) != 0) {
        fclose(playback);
        return PlaybackResult::kFailed;
    }

    ESP_LOGI(TAG, "播放 %s (%u bytes)", path, (unsigned)info.data_bytes);
    RecorderDisplayHideFileMenu();
    RenderAssistant(ui_context);
    RecorderDisplayResume();
    codec.EnableOutput(true);

    bool ok = true;
    uint32_t remaining = info.data_bytes;
    const size_t capacity_samples = kPlaybackReadSamples * info.channels;
    std::vector<int16_t> playback_buf(capacity_samples);
    while (remaining > 0 && !*exit_requested) {
        DrainPlaybackRequests(control, &codec, exit_requested, ui_context);
        if (*exit_requested) {
            break;
        }
        if (control->mode == RecorderControlMode::kPaused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        playback_buf.resize(capacity_samples);
        const size_t to_read = std::min(
            static_cast<size_t>(remaining),
            playback_buf.size() * sizeof(int16_t));
        RecorderDisplayPause();
        const size_t bytes = fread(playback_buf.data(), 1, to_read, playback);
        RecorderDisplayResume();
        if (bytes == 0) {
            ok = false;
            break;
        }
        remaining -= bytes;
        playback_buf.resize(bytes / sizeof(int16_t));
        std::vector<int16_t> converted;
        if (!playback_rate.Process(playback_buf, &converted)) {
            ok = false;
            break;
        }
        if (!converted.empty()) {
            codec.OutputData(converted);
        }
        DrainPlaybackRequests(control, &codec, exit_requested, ui_context);
    }

    while (control->mode == RecorderControlMode::kPaused && !*exit_requested) {
        DrainPlaybackRequests(control, &codec, exit_requested, ui_context);
        if (control->mode == RecorderControlMode::kPaused && !*exit_requested) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (ok && remaining == 0 && !*exit_requested) {
        std::vector<int16_t> converted;
        if (!playback_rate.Flush(&converted)) {
            ok = false;
        } else if (!converted.empty()) {
            codec.OutputData(converted);
        }
    }

    RecorderDisplayPause();
    codec.EnableOutput(false);
    fclose(playback);
    if (*exit_requested) {
        ESP_LOGI(TAG, "播放中断: %s", path);
        return PlaybackResult::kInterrupted;
    }
    return ok && remaining == 0
        ? PlaybackResult::kDone
        : PlaybackResult::kFailed;
}

}  // namespace

void RunRecorderApp() {
    ESP_LOGI(TAG, "recorder app starting");

    // 1. I2C + AXP2101 供电
    if (InitI2cAndPmic() != ESP_OK) {
        ESP_LOGE(TAG, "供电初始化失败，录音 app 退化为空转");
    }

    // 2. 先初始化屏幕（建 SPI2 总线，容忍已建）
    esp_err_t disp_err = RecorderDisplayInit(s_i2c_bus, s_pmic);
    if (disp_err != ESP_OK) {
        ESP_LOGW(TAG, "屏幕初始化失败: %s（录音仍可用，无显示）", esp_err_to_name(disp_err));
    }
    RecorderDisplayCallbacks display_callbacks;
    display_callbacks.record = OnRecordRequested;
    display_callbacks.stop = OnStopRequested;
    display_callbacks.open_menu = OnOpenPlaybackMenu;
    display_callbacks.pause_resume = OnPauseResumeRequested;
    display_callbacks.exit = OnExitRequested;
    display_callbacks.play_file = OnPlayFileRequested;
    RecorderDisplaySetCallbacks(display_callbacks, nullptr);
    RecorderDisplayPause();

    // 3. 再挂 SD 卡（复用屏已建的 SPI2 总线，own_spi_bus=false）
    bool sd_ok = SdCardMount(false);
    if (sd_ok) {
        // Recorder 的屏幕和 SD 卡共用 SPI2，日志 hook 会在任意 ESP_LOG 中写卡，
        // 容易与 LVGL 刷屏并发。录音模式优先保证录制/播放稳定，日志只走串口。
        mkdir(kRecDir, 0777);
        mkdir(kAgentRoot, 0777);
    } else {
        ESP_LOGE(TAG, "SD 卡挂载失败，无法保存录音");
    }
    AgentTurnStore turn_store(kAgentRoot);

    // 4. 音频 codec（读麦克风）
    BoxAudioCodec codec(s_i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
                        AUDIO_INPUT_REFERENCE);
    codec.EnableInput(true);
    const int channels = codec.input_channels();
    const int sample_rate = codec.input_sample_rate();
    ESP_LOGI(TAG, "codec ready: %d Hz, %d ch", sample_rate, channels);
    RecorderNoiseReducer noise_reducer(sample_rate);
    ESP_LOGI(TAG, "recorder DSP: input=%d output=%d ns=%s",
             sample_rate,
             noise_reducer.output_sample_rate(),
             noise_reducer.noise_reduction_enabled() ? "enabled" : "unavailable");

    AgentVoiceState voice_state;
    AgentPendingTurn active_turn;
    bool has_active_turn = false;
    if (sd_ok) {
        const auto pending = turn_store.ListPending();
        if (!pending.empty()) {
            active_turn = pending.front();
            has_active_turn = true;
            voice_state.queued_turn = true;
        }
    }
    RecorderControlState control{RecorderControlMode::kIdle, codec.output_volume()};
    control.voice_phase = voice_state.phase;
    control.voice_turn_pending = voice_state.queued_turn;
    uint32_t assistant_elapsed_seconds = 0;
    RecorderAssistantNotice ui_notice = noise_reducer.valid()
        ? RecorderAssistantNotice::kNone
        : RecorderAssistantNotice::kDspFailure;
    uint64_t ui_notice_until_ms = 0;
    RecorderAssistantRenderContext ui_context = {
        .control = &control,
        .voice_state = &voice_state,
        .notice = &ui_notice,
        .elapsed_seconds = &assistant_elapsed_seconds,
        .sd_ready = sd_ok,
        .noise_reduction_ready = noise_reducer.noise_reduction_enabled(),
    };
    RenderAssistant(&ui_context);
    RecorderDisplayResume();

    RecorderNetwork network;
    RecorderReconnectPolicy reconnect_policy;
    RecorderTurnClock turn_clock;
    network.Start();

    // 5. 实体键只发布事件；状态 reducer 决定当前是否接受。
    static Button left(KEY_LEFT_GPIO);
    left.OnClick([]() {
        PublishRequest(RecorderControlEvent::kPhysicalLeft);
    });

    static Button right(BOOT_BUTTON_GPIO);
    right.OnClick([]() {
        PublishRequest(RecorderControlEvent::kPhysicalRight);
    });

    // 6. 所有文件、DSP 与 codec 操作都留在本任务。
    std::vector<int16_t> buf(kFrameSamples * channels);
    FILE* f = nullptr;
    uint32_t data_bytes = 0;
    bool recording_failed = false;
    bool exit_requested = false;
    int64_t last_disp_ms = 0;
    std::string cur_path;
    AgentTurnPaths recording_paths;
    uint64_t recording_created_at_ms = 0;
    bool wifi_connected = false;
    uint64_t reconnect_at_ms = 0;
    uint32_t heartbeat_seconds = 25;
    uint64_t next_ping_ms = RecorderMonotonicMs() + 10000;
    AgentVoiceControl reply_control;
    uint64_t reply_received_bytes = 0;
    mbedtls_sha256_context reply_digest;
    bool reply_digest_active = false;
    std::string automatic_play_path;

    auto sync_voice_control = [&]() {
        control.voice_phase = voice_state.phase;
        control.voice_turn_pending = voice_state.queued_turn;
    };

    auto abort_reply = [&]() {
        RecorderDisplayPause();
        turn_store.AbortReply();
        RecorderDisplayResume();
        if (reply_digest_active) {
            mbedtls_sha256_free(&reply_digest);
            reply_digest_active = false;
        }
        reply_received_bytes = 0;
        reply_control = {};
    };

    auto load_oldest_pending = [&]() {
        RecorderDisplayPause();
        const auto pending = turn_store.ListPending();
        RecorderDisplayResume();
        if (pending.empty()) {
            active_turn = {};
            has_active_turn = false;
            voice_state.queued_turn = false;
        } else {
            active_turn = pending.front();
            has_active_turn = true;
            voice_state.queued_turn = true;
        }
        sync_voice_control();
    };

    auto send_turn_start = [&]() -> bool {
        if (!has_active_turn || !network.IsSocketConnected()) {
            return false;
        }
        const std::string frame = AgentVoiceBuildTurnStart(
            active_turn.paths.turn_id,
            active_turn.user_bytes,
            active_turn.user_sha256);
        if (frame.empty() || !network.SendText(frame)) {
            return false;
        }
        RecorderDisplayPause();
        const bool updated = turn_store.UpdateState(
            active_turn.paths, AgentTurnStatus::kSending);
        RecorderDisplayResume();
        if (!updated) {
            return false;
        }
        RenderAssistant(&ui_context);
        ESP_LOGI(TAG, "Agent turn queued for upload: %s",
                 active_turn.paths.turn_id.c_str());
        return true;
    };

    auto upload_active_turn = [&](uint32_t chunk_bytes) -> bool {
        if (!has_active_turn || chunk_bytes == 0 ||
            chunk_bytes > kAgentVoiceMaxChunkBytes) {
            return false;
        }
        RecorderDisplayPause();
        FILE* upload = fopen(active_turn.paths.user_wav.c_str(), "rb");
        RecorderDisplayResume();
        if (upload == nullptr) {
            return false;
        }
        std::vector<uint8_t> chunk(chunk_bytes);
        uint64_t sent = 0;
        bool ok = true;
        while (sent < active_turn.user_bytes) {
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(
                chunk.size(), active_turn.user_bytes - sent));
            RecorderDisplayPause();
            const size_t size = fread(chunk.data(), 1, wanted, upload);
            const bool read_failed = ferror(upload) != 0;
            RecorderDisplayResume();
            if (size != wanted || read_failed ||
                !network.SendBinary(chunk.data(), size)) {
                ok = false;
                break;
            }
            sent += size;
        }
        RecorderDisplayPause();
        fclose(upload);
        RecorderDisplayResume();
        if (!ok || sent != active_turn.user_bytes) {
            return false;
        }
        return network.SendText(AgentVoiceBuildTurnEnd(active_turn.paths.turn_id));
    };

    auto process_requests = [&](bool* open_menu, std::string* play_path) {
        sync_voice_control();
        RecorderRequest request;
        while (TakeRequest(&request)) {
            RecorderControlAction action =
                ApplyRequest(request, &control, &codec, &exit_requested, &ui_context);
            if (action == RecorderControlAction::kOpenPlaybackMenu &&
                open_menu != nullptr) {
                *open_menu = true;
            } else if (action == RecorderControlAction::kStartPlayback &&
                       play_path != nullptr) {
                *play_path = request.path;
            }
        }
    };

    auto schedule_reconnect = [&](AgentVoiceEvent event) {
        abort_reply();
        AgentVoiceReduce(&voice_state, event);
        sync_voice_control();
        if (wifi_connected) {
            reconnect_at_ms = RecorderMonotonicMs() + reconnect_policy.NextDelayMs();
        } else {
            reconnect_at_ms = 0;
        }
        RenderAssistant(&ui_context);
    };

    auto fail_connection = [&](const char* reason) {
        ESP_LOGE(TAG, "Agent voice transfer failed: %s", reason);
        network.CloseSocket();
        schedule_reconnect(AgentVoiceEvent::kFailure);
    };

    while (true) {
        const uint64_t now_ms = RecorderMonotonicMs();
        if (ui_notice_until_ms != 0 && now_ms >= ui_notice_until_ms) {
            ui_notice = RecorderAssistantNotice::kNone;
            ui_notice_until_ms = 0;
            RenderAssistant(&ui_context);
        }
        if (wifi_connected && reconnect_at_ms != 0 &&
            now_ms >= reconnect_at_ms) {
            reconnect_at_ms = 0;
            AgentVoiceReduce(&voice_state, AgentVoiceEvent::kWifiConnected);
            sync_voice_control();
            RenderAssistant(&ui_context);
            if (!network.ConnectSocket()) {
                schedule_reconnect(AgentVoiceEvent::kFailure);
            }
        }
        if (network.IsSocketConnected() && now_ms >= next_ping_ms) {
            if (!network.SendText(AgentVoiceBuildPing())) {
                fail_connection("heartbeat send failed");
            }
            next_ping_ms = now_ms +
                std::max<uint64_t>(5000, static_cast<uint64_t>(heartbeat_seconds) * 500ULL);
        }

        RecorderNetworkEvent network_event;
        while (network.Poll(&network_event)) {
            if (network_event.type == RecorderNetworkEventType::kWifiConnected) {
                wifi_connected = true;
                ESP_LOGI(TAG, "Agent voice Wi-Fi connected");
                reconnect_policy.Reset();
                reconnect_at_ms = 0;
                AgentVoiceReduce(&voice_state, AgentVoiceEvent::kWifiConnected);
                sync_voice_control();
                if (ui_notice == RecorderAssistantNotice::kWifiSetup) {
                    ui_notice = RecorderAssistantNotice::kNone;
                }
                RenderAssistant(&ui_context);
                if (!network.ConnectSocket()) {
                    schedule_reconnect(AgentVoiceEvent::kFailure);
                }
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kWifiDisconnected) {
                wifi_connected = false;
                reconnect_at_ms = 0;
                schedule_reconnect(AgentVoiceEvent::kDisconnected);
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kSocketConnected) {
                next_ping_ms = RecorderMonotonicMs() + 10000;
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kSocketDisconnected) {
                schedule_reconnect(AgentVoiceEvent::kDisconnected);
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kNeedsWifiProvisioning) {
                ui_notice = RecorderAssistantNotice::kWifiSetup;
                ui_notice_until_ms = 0;
                RenderAssistant(&ui_context);
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kNeedsAgentProvisioning) {
                ui_notice = RecorderAssistantNotice::kAgentSetup;
                ui_notice_until_ms = 0;
                RenderAssistant(&ui_context);
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kError) {
                schedule_reconnect(AgentVoiceEvent::kFailure);
                continue;
            }
            if (network_event.type == RecorderNetworkEventType::kBinary) {
                if (!has_active_turn || !reply_digest_active ||
                    voice_state.phase != AgentVoicePhase::kReceiving ||
                    network_event.data.empty()) {
                    fail_connection("unexpected reply audio");
                    continue;
                }
                RecorderDisplayPause();
                const bool stored = turn_store.AppendReply(
                    network_event.data.data(), network_event.data.size());
                RecorderDisplayResume();
                if (!stored || mbedtls_sha256_update(
                        &reply_digest,
                        network_event.data.data(),
                        network_event.data.size()) != 0) {
                    fail_connection("reply chunk could not be stored");
                    continue;
                }
                reply_received_bytes += network_event.data.size();
                const std::string acknowledgement = AgentVoiceBuildReplyChunkSaved(
                    active_turn.paths.turn_id, reply_received_bytes);
                if (acknowledgement.empty() || !network.SendText(acknowledgement)) {
                    fail_connection("reply chunk acknowledgement failed");
                }
                continue;
            }
            if (network_event.type != RecorderNetworkEventType::kText) {
                continue;
            }

            AgentVoiceControl frame;
            const std::string expected_turn = has_active_turn
                ? active_turn.paths.turn_id : std::string();
            if (!AgentVoiceParseControl(
                    network_event.text(), expected_turn, &frame)) {
                fail_connection("invalid server control frame");
                continue;
            }
            switch (frame.type) {
                case AgentVoiceControlType::kReady: {
                    ESP_LOGI(TAG, "Agent voice WSS ready");
                    heartbeat_seconds = frame.heartbeat_seconds;
                    if (frame.server_time_ms != 0) {
                        turn_clock.Sync(
                            frame.server_time_ms,
                            frame.timezone_offset_minutes,
                            RecorderMonotonicMs());
                        ESP_LOGI(TAG, "Agent turn clock synced: UTC%+d min",
                                 static_cast<int>(
                                     frame.timezone_offset_minutes));
                    }
                    reconnect_policy.Reset();
                    const AgentVoiceAction action = AgentVoiceReduce(
                        &voice_state, AgentVoiceEvent::kServerReady);
                    sync_voice_control();
                    if (ui_notice == RecorderAssistantNotice::kAgentSetup) {
                        ui_notice = RecorderAssistantNotice::kNone;
                    }
                    RenderAssistant(&ui_context);
                    if (action == AgentVoiceAction::kSendQueuedTurn &&
                        !send_turn_start()) {
                        fail_connection("turn start send failed");
                    }
                    break;
                }
                case AgentVoiceControlType::kTurnReady:
                    if (!upload_active_turn(frame.chunk_bytes)) {
                        fail_connection("WAV upload failed");
                    }
                    break;
                case AgentVoiceControlType::kTurnAccepted: {
                    RecorderDisplayPause();
                    const bool updated = turn_store.UpdateState(
                        active_turn.paths, AgentTurnStatus::kProcessing);
                    RecorderDisplayResume();
                    if (!updated) {
                        fail_connection("turn state could not be stored");
                        break;
                    }
                    AgentVoiceReduce(&voice_state, AgentVoiceEvent::kTurnAccepted);
                    sync_voice_control();
                    RenderAssistant(&ui_context);
                    ESP_LOGI(TAG, "Agent accepted turn: %s",
                             active_turn.paths.turn_id.c_str());
                    break;
                }
                case AgentVoiceControlType::kReplyStart: {
                    abort_reply();
                    RecorderDisplayPause();
                    const bool began = turn_store.BeginReply(
                        active_turn.paths, frame.bytes, frame.sha256);
                    RecorderDisplayResume();
                    if (!began) {
                        fail_connection("reply file could not be created");
                        break;
                    }
                    mbedtls_sha256_init(&reply_digest);
                    if (mbedtls_sha256_starts(&reply_digest, 0) != 0) {
                        mbedtls_sha256_free(&reply_digest);
                        fail_connection("reply hash could not start");
                        break;
                    }
                    reply_digest_active = true;
                    reply_received_bytes = 0;
                    reply_control = frame;
                    AgentVoiceReduce(&voice_state, AgentVoiceEvent::kReplyStarted);
                    sync_voice_control();
                    RenderAssistant(&ui_context);
                    break;
                }
                case AgentVoiceControlType::kReplyEnd: {
                    if (!reply_digest_active ||
                        reply_received_bytes != reply_control.bytes) {
                        fail_connection("reply ended before all bytes arrived");
                        break;
                    }
                    unsigned char digest[32];
                    const bool hash_ok =
                        mbedtls_sha256_finish(&reply_digest, digest) == 0;
                    mbedtls_sha256_free(&reply_digest);
                    reply_digest_active = false;
                    const std::string actual_sha256 = hash_ok
                        ? DigestHex(digest) : std::string();
                    RecorderDisplayPause();
                    const bool committed = hash_ok && turn_store.CommitReply(
                        reply_control.transcript,
                        reply_control.reply_text,
                        reply_received_bytes,
                        actual_sha256,
                        reply_control.server_time);
                    RecorderDisplayResume();
                    if (!committed) {
                        fail_connection("reply integrity check failed");
                        break;
                    }
                    if (!network.SendText(AgentVoiceBuildReplySaved(
                            active_turn.paths.turn_id))) {
                        fail_connection("reply saved acknowledgement failed");
                        break;
                    }
                    AgentVoiceReduce(&voice_state, AgentVoiceEvent::kReplyStored);
                    sync_voice_control();
                    if (RecorderControlReduce(
                            &control, RecorderControlEvent::kAgentReplyReady) ==
                        RecorderControlAction::kStartAgentReplyPlayback) {
                        automatic_play_path = active_turn.paths.assistant_wav;
                    }
                    RenderAssistant(&ui_context);
                    ESP_LOGI(TAG, "Agent reply stored: %s",
                             active_turn.paths.assistant_wav.c_str());
                    break;
                }
                case AgentVoiceControlType::kError: {
                    if (frame.retryable) {
                        fail_connection("server requested retry");
                        break;
                    }
                    abort_reply();
                    RecorderDisplayPause();
                    const bool marked_failed = has_active_turn &&
                        turn_store.MarkFailed(
                            active_turn.paths,
                            frame.error_code.empty()
                                ? "server_rejected_turn"
                                : frame.error_code);
                    RecorderDisplayResume();
                    if (!marked_failed) {
                        fail_connection("rejected turn state could not be stored");
                        break;
                    }
                    ESP_LOGW(TAG, "Agent turn rejected permanently: %s",
                             frame.error_code.c_str());
                    AgentVoiceReduce(
                        &voice_state, AgentVoiceEvent::kTurnRejected);
                    sync_voice_control();
                    load_oldest_pending();
                    ui_notice = frame.error_code == "speech_not_recognized"
                        ? RecorderAssistantNotice::kSpeechNotRecognized
                        : RecorderAssistantNotice::kSaveFailure;
                    ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                    if (has_active_turn && network.IsSocketConnected()) {
                        const AgentVoiceAction action = AgentVoiceReduce(
                            &voice_state, AgentVoiceEvent::kTurnQueued);
                        sync_voice_control();
                        if (action == AgentVoiceAction::kSendQueuedTurn &&
                            !send_turn_start()) {
                            fail_connection("next queued turn could not start");
                        }
                    }
                    RenderAssistant(&ui_context);
                    break;
                }
                case AgentVoiceControlType::kPong:
                    break;
                default:
                    fail_connection("unexpected server state");
                    break;
            }
        }

        bool open_menu = false;
        std::string play_path;
        process_requests(&open_menu, &play_path);

        if (exit_requested) {
            // 退出前若正在录音，先把当前文件收尾保存，避免丢数据
            network.Stop();
            RecorderDisplayPause();
            abort_reply();
            if (f != nullptr) {
                const bool saved_ok = FinishRecording(f, &noise_reducer, &data_bytes) &&
                                      !recording_failed;
                if (!saved_ok) {
                    ESP_LOGE(TAG, "退出时录音保存失败: %s", cur_path.c_str());
                }
                fclose(f);
                f = nullptr;
                if (saved_ok) {
                    uint64_t wav_bytes = 0;
                    std::string wav_sha256;
                    if (!HashSdFile(cur_path, &wav_bytes, &wav_sha256) ||
                        !turn_store.MarkRecorded(
                            recording_paths,
                            wav_bytes,
                            wav_sha256,
                            recording_created_at_ms)) {
                        ESP_LOGE(TAG, "退出时录音清单保存失败: %s", cur_path.c_str());
                    }
                }
            }
            RecorderDisplayResume();
            ESP_LOGI(TAG, "退出录音模式 -> 选择器");
            vTaskDelay(pdMS_TO_TICKS(300));
            AppModeWriteAndReboot(AppMode::kSelector);  // 不返回
        }

        if (open_menu && control.mode == RecorderControlMode::kIdle && f == nullptr) {
            RecorderDisplayPause();
            ShowPlaybackMenu();
            RecorderDisplayResume();
        }

        bool playing_agent_reply = false;
        if (play_path.empty() && !automatic_play_path.empty()) {
            play_path = std::move(automatic_play_path);
            automatic_play_path.clear();
            playing_agent_reply = true;
            AgentVoiceReduce(&voice_state, AgentVoiceEvent::kPlaybackStarted);
            sync_voice_control();
            RenderAssistant(&ui_context);
            ESP_LOGI(TAG, "Agent reply playback start: %s", play_path.c_str());
        }
        if (!play_path.empty() &&
            control.mode == RecorderControlMode::kPlaying && f == nullptr) {
            RecorderDisplayPause();
            const PlaybackResult result =
                PlayWavFile(codec, play_path.c_str(), &control, &exit_requested,
                            &ui_context);
            RecorderControlReduce(&control, RecorderControlEvent::kPlaybackFinished);
            if (playing_agent_reply) {
                AgentVoiceReduce(&voice_state, AgentVoiceEvent::kPlaybackFinished);
                load_oldest_pending();
                if (has_active_turn && network.IsSocketConnected()) {
                    const AgentVoiceAction action = AgentVoiceReduce(
                        &voice_state, AgentVoiceEvent::kTurnQueued);
                    sync_voice_control();
                    if (action == AgentVoiceAction::kSendQueuedTurn &&
                        !send_turn_start()) {
                        fail_connection("next queued turn could not start");
                    }
                }
            }
            if (!exit_requested && result == PlaybackResult::kFailed) {
                ui_notice = RecorderAssistantNotice::kPlaybackFailure;
                ui_notice_until_ms = RecorderMonotonicMs() + 2500;
            }
            RenderAssistant(&ui_context);
            RecorderDisplayResume();
            continue;
        }

        if (control.mode == RecorderControlMode::kRecording && f == nullptr) {
            // 开始录音：新建文件 + 占位头
            RecorderDisplayHideFileMenu();
            assistant_elapsed_seconds = 0;
            ui_notice = RecorderAssistantNotice::kNone;
            ui_notice_until_ms = 0;
            RenderAssistant(&ui_context);
            if (!sd_ok || !SdCardIsMounted()) {
                control.mode = RecorderControlMode::kIdle;
                RenderAssistant(&ui_context);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (!noise_reducer.valid() || !noise_reducer.Reset()) {
                ESP_LOGE(TAG, "录音 DSP 初始化/复位失败");
                control.mode = RecorderControlMode::kIdle;
                ui_notice = RecorderAssistantNotice::kDspFailure;
                ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                RenderAssistant(&ui_context);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (!noise_reducer.noise_reduction_enabled()) {
                ESP_LOGW(TAG, "ESP-SR NS unavailable; recording will only be resampled");
            }
            // 先刷屏，等 LVGL 刷完再动 SD 卡：屏与 SD 卡共用 SPI2 总线，
            // 时间上错开可避免刷屏与写卡抢总线导致的闪屏（短录音尤其明显）。
            RenderAssistant(&ui_context);
            FlushDisplayThenPause();
            const RecorderTurnStamp turn_stamp = turn_clock.MakeStamp(
                RecorderMonotonicMs(), esp_random());
            recording_created_at_ms = turn_stamp.created_at_ms;
            recording_paths = turn_store.Create(
                turn_stamp.date, turn_stamp.turn_id);
            cur_path = recording_paths.user_wav;
            f = recording_paths.valid() ? fopen(cur_path.c_str(), "wb") : nullptr;
            if (f == nullptr) {
                ESP_LOGE(TAG, "无法创建录音文件 %s", cur_path.c_str());
                control.mode = RecorderControlMode::kIdle;
                ui_notice = RecorderAssistantNotice::kSaveFailure;
                ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                RenderAssistant(&ui_context);
                RecorderDisplayResume();
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            WriteWavHeader(f, noise_reducer.output_sample_rate(), 1, 16, 0);
            data_bytes = 0;
            recording_failed = false;
            last_disp_ms = 0;
            ESP_LOGI(TAG, "开始录音 -> %s", cur_path.c_str());
            RecorderDisplayResume();
        }

        if (control.mode == RecorderControlMode::kRecording && f != nullptr) {
            // 读一帧 PCM 并写入
            if (codec.InputData(buf)) {
                std::vector<int16_t> processed;
                bool block_ok = noise_reducer.Process(buf, &processed);
                bool reached_voice_limit = false;
                if (block_ok) {
                    const size_t allowed_samples = AgentVoiceClampPcmSamples(
                        data_bytes, processed.size());
                    reached_voice_limit = allowed_samples < processed.size();
                    processed.resize(allowed_samples);
                }
                RecorderDisplayPause();
                if (block_ok) {
                    block_ok = AppendPcm(f, processed, &data_bytes);
                }
                if (!block_ok) {
                    ESP_LOGE(TAG, "录音 DSP/写入失败: %s", cur_path.c_str());
                    control.mode = RecorderControlMode::kIdle;
                    recording_failed = true;
                    ui_notice = RecorderAssistantNotice::kSaveFailure;
                    ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                } else if (reached_voice_limit) {
                    ESP_LOGI(TAG, "录音达到 Agent 4 MiB 上限，自动停止");
                    control.mode = RecorderControlMode::kIdle;
                }

                // 每约 1 秒刷新一次计时显示
                uint32_t secs = data_bytes /
                    (noise_reducer.output_sample_rate() * sizeof(int16_t));
                int64_t now_ms = (int64_t)secs * 1000;
                if (now_ms - last_disp_ms >= 1000) {
                    last_disp_ms = now_ms;
                    assistant_elapsed_seconds = secs;
                    RenderAssistant(&ui_context);
                }
                RecorderDisplayResume();
            }
            process_requests(nullptr, nullptr);
            if (exit_requested) {
                continue;
            }
        }

        if (control.mode != RecorderControlMode::kRecording && f != nullptr) {
            // 停止录音：回填头 + 关闭
            RecorderDisplayPause();
            const bool saved_ok = FinishRecording(f, &noise_reducer, &data_bytes) &&
                                  !recording_failed;
            fclose(f);
            f = nullptr;
            vTaskDelay(pdMS_TO_TICKS(30));  // 让 SD 写操作在总线上落定，再刷屏，避免抢线
            uint32_t secs = data_bytes /
                (noise_reducer.output_sample_rate() * sizeof(int16_t));
            if (!saved_ok) {
                ESP_LOGE(TAG, "录音保存失败: %s", cur_path.c_str());
                ui_notice = RecorderAssistantNotice::kSaveFailure;
                ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                RenderAssistant(&ui_context);
                RecorderDisplayResume();
                continue;
            }
            uint64_t wav_bytes = 0;
            std::string wav_sha256;
            const bool indexed = HashSdFile(cur_path, &wav_bytes, &wav_sha256) &&
                turn_store.MarkRecorded(
                    recording_paths,
                    wav_bytes,
                    wav_sha256,
                    recording_created_at_ms);
            if (!indexed) {
                ESP_LOGE(TAG, "录音清单保存失败: %s", cur_path.c_str());
                ui_notice = RecorderAssistantNotice::kSaveFailure;
                ui_notice_until_ms = RecorderMonotonicMs() + 2500;
                RenderAssistant(&ui_context);
                RecorderDisplayResume();
                continue;
            }
            ESP_LOGI(TAG, "已保存 %s (%u 秒, %llu 字节)",
                     cur_path.c_str(),
                     static_cast<unsigned>(secs),
                     static_cast<unsigned long long>(wav_bytes));
            ESP_LOGI(TAG, "Agent user WAV stored: %s", cur_path.c_str());
            load_oldest_pending();
            const AgentVoiceAction action = AgentVoiceReduce(
                &voice_state, AgentVoiceEvent::kTurnQueued);
            sync_voice_control();
            RenderAssistant(&ui_context);
            if (action == AgentVoiceAction::kSendQueuedTurn &&
                !send_turn_start()) {
                fail_connection("recorded turn could not start");
            }
            RecorderDisplayResume();
        }

        if (control.mode == RecorderControlMode::kIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 空闲时让出 CPU
        }
    }
}
