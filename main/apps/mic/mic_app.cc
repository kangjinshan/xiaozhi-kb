#include "mic_app.h"

#include "app_mode.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "keyboard_pmic_power_key.h"
#include "mic_controls.h"
#include "mic_display.h"
#include "mic_recording_utils.h"
#include "mic_stereo_processor.h"
#include "mic_wifi_audio.h"
#include "recorder_display.h"
#include "sdcard.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <driver/i2c_master.h>
#include <esp_codec_dev.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "mic_app"

namespace {

constexpr uint8_t kAxp2101Address = 0x34;
constexpr uint8_t kAxp2101Irq1StatusReg = 0x48;
constexpr uint8_t kAxp2101Irq2EnableReg = 0x41;
constexpr uint8_t kAxp2101Irq2StatusReg = 0x49;
constexpr uint8_t kAxp2101Irq3StatusReg = 0x4A;
constexpr uint32_t kPowerKeyPollMs = 25;
constexpr size_t kInputFrames = 480;  // 20 ms at 24 kHz, two channels
constexpr size_t kNetworkFrames = 320;  // 20 ms at 16 kHz, two channels
constexpr uint8_t kChunksPerIdleWindow = 100;  // Let CPU0 idle run every 2 seconds.
constexpr int64_t kStreamStatsIntervalUs = 5 * 1000 * 1000;
constexpr uint32_t kMicTaskStackBytes = 40 * 1024;
constexpr size_t kRecordingBufferBytes = 32 * 1024;
constexpr char kMicDirectory[] = "/sdcard/mic";

i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_pmic = nullptr;
std::atomic<bool> s_power_task_ready{false};
std::atomic<bool> s_screen_on{true};

std::mutex s_request_mutex;
std::deque<MicControlAction> s_requests;

void Publish(MicControlAction request) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    s_requests.push_back(request);
}

bool Take(MicControlAction* request) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    if (request == nullptr || s_requests.empty()) {
        return false;
    }
    *request = s_requests.front();
    s_requests.pop_front();
    return true;
}

esp_err_t WriteReg(i2c_master_dev_handle_t device,
                   uint8_t reg,
                   uint8_t value) {
    const uint8_t buffer[] = {reg, value};
    return i2c_master_transmit(device, buffer, sizeof(buffer), 100);
}

esp_err_t ReadReg(i2c_master_dev_handle_t device,
                  uint8_t reg,
                  uint8_t* value) {
    return i2c_master_transmit_receive(
        device, &reg, sizeof(reg), value, sizeof(*value), 100);
}

esp_err_t ClearPowerIrq() {
    esp_err_t err = WriteReg(s_pmic, kAxp2101Irq1StatusReg, 0xFF);
    if (err == ESP_OK) {
        err = WriteReg(s_pmic, kAxp2101Irq2StatusReg, 0xFF);
    }
    if (err == ESP_OK) {
        err = WriteReg(s_pmic, kAxp2101Irq3StatusReg, 0xFF);
    }
    return err;
}

esp_err_t InitI2cAndPmic() {
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    bus_config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kAxp2101Address;
    device_config.scl_speed_hz = 400 * 1000;
    err = i2c_master_bus_add_device(
        s_i2c_bus, &device_config, &s_pmic);
    if (err != ESP_OK) {
        return err;
    }

    const struct {
        uint8_t reg;
        uint8_t value;
    } power_sequence[] = {
        {0x22, 0x06}, {0x27, 0x10}, {0x80, 0x01},
        {0x90, 0x00}, {0x91, 0x00}, {0x82, 18},
        {0x92, 28}, {0x93, 28}, {0x94, 28}, {0x95, 28},
        {0x90, 0x0F}, {0x64, 0x02}, {0x61, 0x02},
        {0x62, 0x0A}, {0x63, 0x01},
    };
    for (const auto& item : power_sequence) {
        err = WriteReg(s_pmic, item.reg, item.value);
        if (err != ESP_OK) {
            return err;
        }
    }

    uint8_t irq_enable = 0;
    err = ReadReg(s_pmic, kAxp2101Irq2EnableReg, &irq_enable);
    if (err == ESP_OK) {
        err = WriteReg(s_pmic,
                       kAxp2101Irq2EnableReg,
                       irq_enable | Axp2101PowerKeyIrqEnableMask());
    }
    if (err == ESP_OK) {
        err = ClearPowerIrq();
    }
    s_power_task_ready.store(err == ESP_OK);
    return ESP_OK;
}

void PowerKeyTask(void*) {
    bool latched = true;
    while (true) {
        uint8_t status = 0;
        if (ReadReg(s_pmic, kAxp2101Irq2StatusReg, &status) == ESP_OK) {
            const bool pressed = IsAxp2101PowerKeyToggleEvent(status);
            if (status != 0) {
                ClearPowerIrq();
            }
            if (!pressed) {
                latched = false;
            } else if (!latched) {
                latched = true;
                Publish(MicActionForInput(MicControlInput::kPhysicalPower));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPowerKeyPollMs));
    }
}

bool WriteWavHeader(FILE* file, uint32_t data_bytes) {
    if (file == nullptr || std::fseek(file, 0, SEEK_SET) != 0) {
        return false;
    }
    const auto header = MicBuildStereoWavHeader(data_bytes);
    return std::fwrite(header.data(), 1, header.size(), file) == header.size();
}

std::string NextRecordingPath() {
    for (int index = 1; index <= 9999; ++index) {
        char path[64];
        std::snprintf(path, sizeof(path), "%s/mic%04d.wav",
                      kMicDirectory, index);
        if (access(path, F_OK) != 0) {
            return path;
        }
    }
    return {};
}

void Render(MicDisplayModel* model, const MicWifiAudio& wifi) {
    model->wifi_connected = wifi.connected();
    model->wifi_streaming = wifi.streaming();
    MicDisplayRender(*model);
}

void OnToggleRecording(void*) {
    Publish(MicActionForInput(MicControlInput::kTouchRecording));
}

void OnExit(void*) {
    Publish(MicActionForInput(MicControlInput::kTouchMenu));
}

void MicTask(void*) {
    RunMicApp();
    vTaskDelete(nullptr);
}

}  // namespace

bool StartMicApp() {
    const BaseType_t result = xTaskCreate(
        MicTask, "mic_app", kMicTaskStackBytes, nullptr, 1, nullptr);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "failed to create Mic task");
        return false;
    }
    ESP_LOGI(TAG, "Mic task created stack=%lu bytes",
             static_cast<unsigned long>(kMicTaskStackBytes));
    return true;
}

void RunMicApp() {
    ESP_LOGI(TAG, "real-time stereo Wi-Fi microphone starting");
    if (InitI2cAndPmic() != ESP_OK) {
        ESP_LOGE(TAG, "I2C/PMIC initialization failed");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const bool display_ready =
        RecorderDisplayInit(s_i2c_bus, s_pmic) == ESP_OK;
    if (!display_ready) {
        ESP_LOGE(TAG, "display initialization failed");
    }
    RecorderDisplayPause();
    bool sd_ready = SdCardMount(false);
    if (sd_ready) {
        sd_ready = mkdir(kMicDirectory, 0777) == 0 || errno == EEXIST;
        if (!sd_ready) {
            ESP_LOGE(TAG, "failed to create %s", kMicDirectory);
        }
    }
    RecorderDisplayResume();

    const MicDisplayCallbacks display_callbacks = {
        .toggle_recording = OnToggleRecording,
        .exit = OnExit,
    };
    if (display_ready && !MicDisplayInit(display_callbacks, nullptr)) {
        ESP_LOGE(TAG, "microphone UI initialization failed");
    }

    constexpr uint16_t kStereoMicMask =
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    BoxAudioCodec codec(
        s_i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
        false, kStereoMicMask);
    codec.EnableInput(true);
    int gain_percent = 70;
    codec.SetInputGain(MicInputGainDb(gain_percent));
    ESP_LOGI(TAG, "ES7210 stereo input: MIC1=L MIC2=R rate=%d channels=%d",
             codec.input_sample_rate(), codec.input_channels());

    MicStereoProcessor processor(
        codec.input_sample_rate(), nullptr, nullptr, true);
    MicWifiAudio wifi;
    const bool wifi_ready = wifi.Init();
    ESP_LOGI(TAG, "microphone DSP shared-mid-ns=%s Wi-Fi=%s",
             processor.noise_reduction_enabled() ? "enabled" : "unavailable",
             wifi_ready ? "starting" : "unavailable");

    static Button key(KEY_LEFT_GPIO);
    key.OnClick([]() {
        Publish(MicActionForInput(MicControlInput::kPhysicalKey));
    });
    static Button boot(BOOT_BUTTON_GPIO);
    boot.OnClick([]() {
        Publish(MicActionForInput(MicControlInput::kPhysicalBoot));
    });
    if (s_power_task_ready.load()) {
        xTaskCreate(PowerKeyTask, "mic_pwr", 3072, nullptr, 5, nullptr);
    }

    MicDisplayModel model;
    model.sd_ready = sd_ready;
    model.noise_reduction = processor.noise_reduction_enabled();
    model.gain_percent = gain_percent;
    Render(&model, wifi);

    std::vector<int16_t> input(kInputFrames * 2);
    std::vector<int16_t> network_pending;
    FILE* recording = nullptr;
    uint32_t data_bytes = 0;
    int64_t recording_started_us = 0;
    uint32_t last_rendered_second = UINT32_MAX;
    bool last_connected = false;
    bool last_streaming = false;
    bool stack_margin_logged = false;
    uint8_t chunks_until_idle = kChunksPerIdleWindow;
    bool stream_was_active = false;
    int64_t stream_stats_started_us = 0;
    uint32_t stream_stats_sent = 0;
    uint32_t stream_stats_dropped = 0;
    uint64_t stream_stats_send_us = 0;
    uint32_t stream_stats_send_calls = 0;
    int64_t capture_stats_started_us = esp_timer_get_time();
    uint64_t capture_stats_read_us = 0;
    uint64_t capture_stats_dsp_us = 0;
    uint32_t capture_stats_chunks = 0;
    uint32_t capture_stats_frames = 0;

    auto stop_recording = [&]() {
        if (recording == nullptr) {
            return;
        }
        RecorderDisplayPause();
        const bool header_ok = WriteWavHeader(recording, data_bytes);
        const bool flush_ok = std::fflush(recording) == 0;
        const bool sync_ok = fsync(fileno(recording)) == 0;
        const bool close_ok = std::fclose(recording) == 0;
        RecorderDisplayResume();
        recording = nullptr;
        model.recording = false;
        if (header_ok && flush_ok && sync_ok && close_ok) {
            ESP_LOGI(TAG, "recording saved: %s bytes=%lu",
                     model.filename.c_str(),
                     static_cast<unsigned long>(data_bytes));
        } else {
            ESP_LOGE(TAG, "recording finalization failed: %s",
                     model.filename.c_str());
        }
    };

    while (true) {
        MicControlAction request;
        while (Take(&request)) {
            switch (request) {
                case MicControlAction::kToggleRecording:
                    if (recording != nullptr) {
                        stop_recording();
                    } else if (sd_ready) {
                        RecorderDisplayPause();
                        model.filename = NextRecordingPath();
                        recording = model.filename.empty()
                            ? nullptr
                            : std::fopen(model.filename.c_str(), "wb");
                        if (recording != nullptr &&
                            std::setvbuf(recording, nullptr, _IOFBF,
                                        kRecordingBufferBytes) != 0) {
                            ESP_LOGE(TAG, "failed to allocate WAV buffer");
                            std::fclose(recording);
                            recording = nullptr;
                            unlink(model.filename.c_str());
                        }
                        if (recording != nullptr &&
                            !WriteWavHeader(recording, 0)) {
                            std::fclose(recording);
                            recording = nullptr;
                            unlink(model.filename.c_str());
                        }
                        RecorderDisplayResume();
                        if (recording != nullptr) {
                            data_bytes = 0;
                            recording_started_us = esp_timer_get_time();
                            model.elapsed_seconds = 0;
                            model.recording = true;
                            ESP_LOGI(TAG, "recording started: %s",
                                     model.filename.c_str());
                        }
                    }
                    Render(&model, wifi);
                    break;
                case MicControlAction::kGainUp:
                    gain_percent = MicGainAfterAction(gain_percent, request);
                    codec.SetInputGain(MicInputGainDb(gain_percent));
                    model.gain_percent = gain_percent;
                    ESP_LOGI(TAG, "KEY gain up: %d%%", gain_percent);
                    Render(&model, wifi);
                    break;
                case MicControlAction::kGainDown:
                    gain_percent = MicGainAfterAction(gain_percent, request);
                    codec.SetInputGain(MicInputGainDb(gain_percent));
                    model.gain_percent = gain_percent;
                    ESP_LOGI(TAG, "BOOT gain down: %d%%", gain_percent);
                    Render(&model, wifi);
                    break;
                case MicControlAction::kToggleScreen: {
                    const bool turn_on = !s_screen_on.load();
                    if (RecorderDisplaySetScreenOn(turn_on) == ESP_OK) {
                        s_screen_on.store(turn_on);
                    }
                    break;
                }
                case MicControlAction::kExit:
                    stop_recording();
                    ESP_LOGI(TAG, "MENU -> selector");
                    AppModeWriteAndReboot(AppMode::kSelector);
                    break;
            }
        }

        const int64_t read_started_us = esp_timer_get_time();
        if (!codec.InputData(input)) {
            // CONFIG_FREERTOS_HZ is 100, so pdMS_TO_TICKS(5) is zero and
            // would spin at main-task priority without ever running IDLE0.
            vTaskDelay(1);
            continue;
        }
        const int64_t read_finished_us = esp_timer_get_time();
        std::vector<int16_t> processed;
        if (!processor.Process(input, &processed)) {
            ESP_LOGE(TAG, "stereo DSP failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const int64_t dsp_finished_us = esp_timer_get_time();
        capture_stats_read_us += read_finished_us - read_started_us;
        capture_stats_dsp_us += dsp_finished_us - read_finished_us;
        ++capture_stats_chunks;
        capture_stats_frames += processed.size() / 2;

        // Live transport is the primary Mic function. Send this 20 ms block
        // before doing any optional SD work so START/STOP never changes the
        // stream into a record-then-send path.
        network_pending.insert(
            network_pending.end(), processed.begin(), processed.end());
        const size_t network_samples = kNetworkFrames * 2;
        while (network_pending.size() >= network_samples) {
            const bool stream_active = wifi.streaming();
            const int64_t send_started_us = esp_timer_get_time();
            wifi.SendPcm(network_pending.data(), kNetworkFrames);
            if (stream_active) {
                stream_stats_send_us +=
                    esp_timer_get_time() - send_started_us;
                ++stream_stats_send_calls;
            }
            if (stream_active && !stack_margin_logged) {
                stack_margin_logged = true;
                ESP_LOGI(TAG, "Mic task stack minimum free=%u bytes",
                         static_cast<unsigned>(
                             uxTaskGetStackHighWaterMark(nullptr)));
            }
            network_pending.erase(network_pending.begin(),
                                  network_pending.begin() + network_samples);
        }

        if (recording != nullptr && !processed.empty()) {
            RecorderDisplayPause();
            const size_t written = std::fwrite(
                processed.data(), sizeof(int16_t), processed.size(), recording);
            RecorderDisplayResume();
            data_bytes += static_cast<uint32_t>(written * sizeof(int16_t));
            if (written != processed.size()) {
                ESP_LOGE(TAG, "SD write failed");
                stop_recording();
            }
        }

        const bool stream_active = wifi.streaming();
        const int64_t now_us = esp_timer_get_time();
        if (stream_active && !stream_was_active) {
            stream_stats_started_us = now_us;
            stream_stats_sent = wifi.sent_packets();
            stream_stats_dropped = wifi.dropped_packets();
            stream_stats_send_us = 0;
            stream_stats_send_calls = 0;
        } else if (stream_active &&
                   now_us - stream_stats_started_us >=
                       kStreamStatsIntervalUs) {
            const uint32_t sent = wifi.sent_packets();
            const uint32_t dropped = wifi.dropped_packets();
            const uint32_t sent_delta = sent - stream_stats_sent;
            const uint32_t dropped_delta = dropped - stream_stats_dropped;
            const float wall_seconds = static_cast<float>(
                now_us - stream_stats_started_us) / 1000000.0f;
            const float audio_seconds = sent_delta * 0.020f;
            const uint32_t average_send_us = stream_stats_send_calls == 0
                ? 0
                : stream_stats_send_us / stream_stats_send_calls;
            ESP_LOGI(TAG,
                     "Wi-Fi realtime %.2fx audio=%.2fs wall=%.2fs "
                     "sent=%lu dropped=%lu send_avg=%luus",
                     audio_seconds / wall_seconds,
                     audio_seconds,
                     wall_seconds,
                     static_cast<unsigned long>(sent_delta),
                     static_cast<unsigned long>(dropped_delta),
                     static_cast<unsigned long>(average_send_us));
            stream_stats_started_us = now_us;
            stream_stats_sent = sent;
            stream_stats_dropped = dropped;
            stream_stats_send_us = 0;
            stream_stats_send_calls = 0;
        }
        stream_was_active = stream_active;

        if (now_us - capture_stats_started_us >= kStreamStatsIntervalUs) {
            const float wall_seconds = static_cast<float>(
                now_us - capture_stats_started_us) / 1000000.0f;
            const float audio_seconds =
                static_cast<float>(capture_stats_frames) / 16000.0f;
            const uint32_t average_read_us = capture_stats_chunks == 0
                ? 0
                : capture_stats_read_us / capture_stats_chunks;
            const uint32_t average_dsp_us = capture_stats_chunks == 0
                ? 0
                : capture_stats_dsp_us / capture_stats_chunks;
            ESP_LOGI(TAG,
                     "capture realtime %.2fx audio=%.2fs wall=%.2fs "
                     "read_avg=%luus dsp_avg=%luus wifi=%s stream=%s",
                     audio_seconds / wall_seconds,
                     audio_seconds,
                     wall_seconds,
                     static_cast<unsigned long>(average_read_us),
                     static_cast<unsigned long>(average_dsp_us),
                     wifi.connected() ? "connected" : "offline",
                     wifi.streaming() ? "active" : "waiting");
            capture_stats_started_us = now_us;
            capture_stats_read_us = 0;
            capture_stats_dsp_us = 0;
            capture_stats_chunks = 0;
            capture_stats_frames = 0;
        }

        if (model.recording) {
            const uint32_t elapsed = static_cast<uint32_t>(
                (esp_timer_get_time() - recording_started_us) / 1000000);
            if (elapsed != last_rendered_second) {
                last_rendered_second = elapsed;
                model.elapsed_seconds = elapsed;
                Render(&model, wifi);
            }
        }
        if (last_connected != wifi.connected() ||
            last_streaming != wifi.streaming()) {
            last_connected = wifi.connected();
            last_streaming = wifi.streaming();
            Render(&model, wifi);
        }

        // Shared-mid NS is close to one full core. Periodically block for one
        // 10 ms tick so IDLE0 can reset the task watchdog while keeping the
        // capture rate near 1.00x.
        if (--chunks_until_idle == 0) {
            chunks_until_idle = kChunksPerIdleWindow;
            vTaskDelay(1);
        }
    }
}
