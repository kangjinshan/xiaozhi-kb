#include "recorder_app.h"
#include "recorder_display.h"

#include "config.h"
#include "sdcard.h"
#include "button.h"
#include "app_mode.h"
#include "codecs/box_audio_codec.h"
#include "recorder_file_list.h"
#include "recorder_control_state.h"
#include "recorder_noise_reducer.h"
#include "recorder_rate_converter.h"
#include "recorder_wav_file.h"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <mbedtls/base64.h>

#define TAG "recorder_app"

namespace {

constexpr uint8_t kAxp2101Address = 0x34;
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
const char* kRecDir = "/sdcard/rec";
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

// 扫描 /sdcard/rec，返回下一个可用 recNNN.wav 序号
int NextRecIndex() {
    int max_idx = -1;
    DIR* dir = opendir(kRecDir);
    if (dir != nullptr) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            int idx = -1;
            if (sscanf(ent->d_name, "rec%d.wav", &idx) == 1 && idx > max_idx) {
                max_idx = idx;
            }
        }
        closedir(dir);
    }
    return max_idx + 1;
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
    return std::fflush(file) == 0;
}

// 把 WAV 文件经串口 base64 回传（免拔卡即可在电脑端还原分析）。
// 用固定标记包裹，电脑端脚本据此提取。数据走 printf 直出 stdout，
// 不经 ESP_LOG（避免被日志 hook 加前缀/写卡）。
void DumpWavOverSerial(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    printf("\n<<<WAV_BEGIN %s %ld>>>\n", path, total);
    unsigned char in[900];   // 900 是 3 的倍数，base64 不产生跨块填充
    unsigned char out[1220];
    size_t n;
    while ((n = fread(in, 1, sizeof(in), f)) > 0) {
        size_t olen = 0;
        if (mbedtls_base64_encode(out, sizeof(out), &olen, in, n) == 0) {
            fwrite(out, 1, olen, stdout);
            fputc('\n', stdout);
        }
    }
    fclose(f);
    printf("<<<WAV_END>>>\n");
    fflush(stdout);
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

const char* Basename(const char* path) {
    if (path == nullptr) {
        return "";
    }
    const char* slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

RecorderDisplayState ToDisplayState(RecorderControlMode mode) {
    switch (mode) {
        case RecorderControlMode::kIdle:
            return RecorderDisplayState::kIdle;
        case RecorderControlMode::kRecording:
            return RecorderDisplayState::kRecording;
        case RecorderControlMode::kPlaying:
            return RecorderDisplayState::kPlaying;
        case RecorderControlMode::kPaused:
            return RecorderDisplayState::kPaused;
    }
    return RecorderDisplayState::kIdle;
}

RecorderControlAction ApplyRequest(const RecorderRequest& request,
                                   RecorderControlState* control,
                                   BoxAudioCodec* codec,
                                   bool* exit_requested) {
    RecorderControlAction action = RecorderControlReduce(control, request.event);
    if (action == RecorderControlAction::kVolumeChanged && codec != nullptr) {
        codec->SetOutputVolume(control->volume);
        RecorderDisplaySetState(ToDisplayState(control->mode), control->volume);
    } else if (action == RecorderControlAction::kPausePlayback ||
               action == RecorderControlAction::kResumePlayback) {
        RecorderDisplaySetState(ToDisplayState(control->mode), control->volume);
    } else if (action == RecorderControlAction::kExit && exit_requested != nullptr) {
        *exit_requested = true;
    }
    return action;
}

void DrainPlaybackRequests(RecorderControlState* control,
                           BoxAudioCodec* codec,
                           bool* exit_requested) {
    RecorderRequest request;
    while (TakeRequest(&request)) {
        ApplyRequest(request, control, codec, exit_requested);
    }
}

void FlushDisplayThenPause(uint32_t ms = 90) {
    RecorderDisplayResume();
    vTaskDelay(pdMS_TO_TICKS(ms));
    RecorderDisplayPause();
}

void ShowPlaybackMenu() {
    std::vector<RecorderDisplayMenuItem> menu_items;
    auto entries = RecorderListRecordings(kRecDir, 32);
    menu_items.reserve(entries.size());
    for (const auto& entry : entries) {
        RecorderDisplayMenuItem item;
        item.label = entry.name;
        item.detail = RecorderFormatRecordingDetail(entry);
        item.path = entry.path;
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
                           bool* exit_requested) {
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
    RecorderShowText("PLAYING", nullptr);
    RecorderDisplayHideFileMenu();
    RecorderDisplaySetState(RecorderDisplayState::kPlaying, control->volume);
    RecorderDisplayResume();
    codec.EnableOutput(true);

    bool ok = true;
    uint32_t remaining = info.data_bytes;
    const size_t capacity_samples = kPlaybackReadSamples * info.channels;
    std::vector<int16_t> playback_buf(capacity_samples);
    while (remaining > 0 && !*exit_requested) {
        DrainPlaybackRequests(control, &codec, exit_requested);
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
        DrainPlaybackRequests(control, &codec, exit_requested);
    }

    while (control->mode == RecorderControlMode::kPaused && !*exit_requested) {
        DrainPlaybackRequests(control, &codec, exit_requested);
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
    } else {
        ESP_LOGE(TAG, "SD 卡挂载失败，无法保存录音");
    }

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

    const char* idle_subtitle = noise_reducer.noise_reduction_enabled()
        ? "NS READY / tap REC"
        : "NS OFF / tap REC";
    if (!noise_reducer.valid()) {
        idle_subtitle = "NS FAILED";
    }
    RecorderControlState control{RecorderControlMode::kIdle, codec.output_volume()};
    RecorderShowText("REC 00:00", sd_ok ? idle_subtitle : "NO SD CARD");
    RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
    RecorderDisplayResume();

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
    char cur_path[64] = {0};

    auto process_requests = [&](bool* open_menu, std::string* play_path) {
        RecorderRequest request;
        while (TakeRequest(&request)) {
            RecorderControlAction action =
                ApplyRequest(request, &control, &codec, &exit_requested);
            if (action == RecorderControlAction::kOpenPlaybackMenu &&
                open_menu != nullptr) {
                *open_menu = true;
            } else if (action == RecorderControlAction::kStartPlayback &&
                       play_path != nullptr) {
                *play_path = request.path;
            }
        }
    };

    while (true) {
        bool open_menu = false;
        std::string play_path;
        process_requests(&open_menu, &play_path);

        if (exit_requested) {
            // 退出前若正在录音，先把当前文件收尾保存，避免丢数据
            RecorderDisplayPause();
            if (f != nullptr) {
                const bool saved_ok = FinishRecording(f, &noise_reducer, &data_bytes) &&
                                      !recording_failed;
                if (!saved_ok) {
                    ESP_LOGE(TAG, "退出时录音保存失败: %s", cur_path);
                    RecorderShowText("SAVE FAILED", "write error");
                }
                fclose(f);
                f = nullptr;
            }
            RecorderShowText("EXIT", "back to menu");
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

        if (!play_path.empty() &&
            control.mode == RecorderControlMode::kPlaying && f == nullptr) {
            RecorderDisplayPause();
            const PlaybackResult result =
                PlayWavFile(codec, play_path.c_str(), &control, &exit_requested);
            RecorderControlReduce(&control, RecorderControlEvent::kPlaybackFinished);
            RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
            if (!exit_requested) {
                if (result == PlaybackResult::kDone) {
                    RecorderShowText("PLAY DONE", Basename(play_path.c_str()));
                } else if (result == PlaybackResult::kFailed) {
                    RecorderShowText("PLAY FAILED", "check wav");
                }
            }
            RecorderDisplayResume();
            continue;
        }

        if (control.mode == RecorderControlMode::kRecording && f == nullptr) {
            // 开始录音：新建文件 + 占位头
            RecorderDisplayHideFileMenu();
            RecorderDisplaySetState(RecorderDisplayState::kRecording, control.volume);
            if (!sd_ok || !SdCardIsMounted()) {
                RecorderShowText("NO SD CARD", "cannot record");
                control.mode = RecorderControlMode::kIdle;
                RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (!noise_reducer.valid() || !noise_reducer.Reset()) {
                ESP_LOGE(TAG, "录音 DSP 初始化/复位失败");
                RecorderShowText("NS FAILED", "cannot record");
                control.mode = RecorderControlMode::kIdle;
                RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            if (!noise_reducer.noise_reduction_enabled()) {
                ESP_LOGW(TAG, "ESP-SR NS unavailable; recording will only be resampled");
            }
            // 先刷屏，等 LVGL 刷完再动 SD 卡：屏与 SD 卡共用 SPI2 总线，
            // 时间上错开可避免刷屏与写卡抢总线导致的闪屏（短录音尤其明显）。
            RecorderShowText("REC 00:00",
                             noise_reducer.noise_reduction_enabled() ? "recording" : "NS OFF");
            FlushDisplayThenPause();
            int idx = NextRecIndex();
            snprintf(cur_path, sizeof(cur_path), "%s/rec%d.wav", kRecDir, idx);
            f = fopen(cur_path, "wb");
            if (f == nullptr) {
                ESP_LOGE(TAG, "无法创建录音文件 %s", cur_path);
                RecorderShowText("SAVE FAILED", "write error");
                control.mode = RecorderControlMode::kIdle;
                RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
                RecorderDisplayResume();
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            WriteWavHeader(f, noise_reducer.output_sample_rate(), 1, 16, 0);
            data_bytes = 0;
            recording_failed = false;
            last_disp_ms = 0;
            ESP_LOGI(TAG, "开始录音 -> %s", cur_path);
            RecorderDisplayResume();
        }

        if (control.mode == RecorderControlMode::kRecording && f != nullptr) {
            // 读一帧 PCM 并写入
            if (codec.InputData(buf)) {
                std::vector<int16_t> processed;
                bool block_ok = noise_reducer.Process(buf, &processed);
                RecorderDisplayPause();
                if (block_ok) {
                    block_ok = AppendPcm(f, processed, &data_bytes);
                }
                if (!block_ok) {
                    ESP_LOGE(TAG, "录音 DSP/写入失败: %s", cur_path);
                    control.mode = RecorderControlMode::kIdle;
                    recording_failed = true;
                }

                // 每约 1 秒刷新一次计时显示
                uint32_t secs = data_bytes /
                    (noise_reducer.output_sample_rate() * sizeof(int16_t));
                int64_t now_ms = (int64_t)secs * 1000;
                if (now_ms - last_disp_ms >= 1000) {
                    last_disp_ms = now_ms;
                    char t[16];
                    snprintf(t, sizeof(t), "REC %02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
                    RecorderShowText(t, "tap STOP to save");
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
                ESP_LOGE(TAG, "录音保存失败: %s", cur_path);
                RecorderShowText("SAVE FAILED", "write error");
                RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
                RecorderDisplayResume();
                continue;
            }
            ESP_LOGI(TAG, "已保存 %s (%u 秒, %u 字节)", cur_path, (unsigned)secs, (unsigned)data_bytes);
            char saved[48];
            snprintf(saved, sizeof(saved), "SAVED %02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
            RecorderShowText(saved, "sending");
            DumpWavOverSerial(cur_path);  // 串口回传，便于电脑端免拔卡分析
            RecorderShowText(saved, idle_subtitle);
            RecorderDisplaySetState(RecorderDisplayState::kIdle, control.volume);
            RecorderDisplayResume();
        }

        if (control.mode == RecorderControlMode::kIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 空闲时让出 CPU
        }
    }
}
