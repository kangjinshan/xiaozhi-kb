# ESP32-C6 Recorder ESP-SR Noise Suppression Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ESP32-C6 recorder's sample gate with the official low-memory ESP-SR noise suppressor and save its 16 kHz enhanced PCM while retaining playback compatibility with existing 24 kHz files.

**Architecture:** Keep `BoxAudioCodec` at 24 kHz, wrap the existing `esp_ae_rate_cvt` library in a reusable streaming converter, and feed exact 160-sample frames to ESP-SR `ns_create(10)`/`ns_process()`. The recorder writes 16 kHz WAV data; playback uses the same converter to play both 16 kHz and native 24 kHz files through the 24 kHz codec.

**Tech Stack:** C++17, ESP-IDF 5.5.3, ESP-SR 2.3.1, `esp_audio_effects` rate conversion, host `c++` tests, ESP32-C6 hardware validation.

---

## File structure

- Create `main/apps/recorder/recorder_rate_converter.h`: platform-neutral streaming converter interface.
- Create `main/apps/recorder/recorder_rate_converter.cc`: `esp_ae_rate_cvt` device backend, deterministic host backend, exact output-length accounting, reset and flush.
- Create `main/apps/recorder/recorder_rate_converter_test.cc`: chunk-boundary and sample-count tests.
- Rewrite `main/apps/recorder/recorder_noise_reducer.h`: input/output API, injectable frame processor, explicit state and status.
- Rewrite `main/apps/recorder/recorder_noise_reducer.cc`: 24 kHz→16 kHz conversion, 160-sample ESP-SR frames, fixed post-gain and limiter, heap diagnostics.
- Rewrite `main/apps/recorder/recorder_noise_reducer_test.cc`: tests frame dispatch, tail handling, fallback, gain and limiting without pretending a threshold gate is noise reduction.
- Modify `main/apps/recorder/recorder_wav_file.h` and `.cc`: identify playable 16/24 kHz mono PCM WAV files.
- Modify `main/apps/recorder/recorder_playback_menu_test.cc`: cover both rates and reject unsupported formats.
- Modify `main/apps/recorder/recorder_app.cc`: write enhanced 16 kHz output, flush on both stop paths, and resample 16 kHz playback.
- Modify `main/CMakeLists.txt`: compile the converter.
- Modify `README.md` and `docs/recorder-design-guardrails.md`: record the actual C6 implementation and hardware result.

### Task 1: Streaming sample-rate converter

**Files:**
- Create: `main/apps/recorder/recorder_rate_converter.h`
- Create: `main/apps/recorder/recorder_rate_converter.cc`
- Create: `main/apps/recorder/recorder_rate_converter_test.cc`
- Modify: `main/CMakeLists.txt:61-68`

- [ ] **Step 1: Write the failing converter test**

Create `recorder_rate_converter_test.cc` with these complete cases:

```cpp
#include "recorder_rate_converter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::vector<int16_t> ConvertInChunks(size_t input_samples,
                                     int source_rate,
                                     int destination_rate,
                                     size_t chunk_size) {
    RecorderRateConverter converter(source_rate, destination_rate);
    Check(converter.valid(), "converter initializes");
    std::vector<int16_t> input(input_samples);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<int16_t>(static_cast<int>(i % 2000) - 1000);
    }

    std::vector<int16_t> output;
    for (size_t offset = 0; offset < input.size(); offset += chunk_size) {
        const size_t count = std::min(chunk_size, input.size() - offset);
        std::vector<int16_t> block(input.begin() + offset, input.begin() + offset + count);
        Check(converter.Process(block, &output), "chunk converts");
    }
    Check(converter.Flush(&output), "converter flushes");
    return output;
}
}  // namespace

int main() {
    const auto down_single = ConvertInChunks(2400, 24000, 16000, 2400);
    const auto down_chunked = ConvertInChunks(2400, 24000, 16000, 317);
    Check(down_single.size() == 1600, "24k to 16k count");
    Check(down_chunked.size() == 1600, "chunked downsample count");
    Check(down_single == down_chunked, "downsample is chunk invariant");

    const auto up = ConvertInChunks(1600, 16000, 24000, 173);
    Check(up.size() == 2400, "16k to 24k count");

    RecorderRateConverter passthrough(24000, 24000);
    std::vector<int16_t> input = {1, -2, 3, -4};
    std::vector<int16_t> output;
    Check(passthrough.Process(input, &output), "passthrough converts");
    Check(passthrough.Flush(&output), "passthrough flushes");
    Check(output == input, "same-rate samples are unchanged");

    RecorderRateConverter invalid(0, 16000);
    Check(!invalid.valid(), "invalid rate is rejected");
    std::puts("recorder_rate_converter_test passed");
    return 0;
}
```

- [ ] **Step 2: Run the converter test and verify RED**

Run:

```bash
cd /Users/kanayama/xiaozhi-kb
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_rate_converter_test.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/recorder_rate_converter_test
```

Expected: compilation fails because `recorder_rate_converter.*` do not exist.

- [ ] **Step 3: Add the converter interface**

Create `recorder_rate_converter.h` with this public contract:

```cpp
#ifndef RECORDER_RATE_CONVERTER_H_
#define RECORDER_RATE_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

class RecorderRateConverter {
public:
    RecorderRateConverter(int source_rate, int destination_rate);
    ~RecorderRateConverter();

    RecorderRateConverter(const RecorderRateConverter&) = delete;
    RecorderRateConverter& operator=(const RecorderRateConverter&) = delete;

    bool valid() const { return valid_; }
    int source_rate() const { return source_rate_; }
    int destination_rate() const { return destination_rate_; }
    bool Process(const std::vector<int16_t>& input, std::vector<int16_t>* output);
    bool Flush(std::vector<int16_t>* output);
    bool Reset();

private:
    int source_rate_;
    int destination_rate_;
    bool valid_ = false;
    bool flushed_ = false;
    uint64_t total_input_samples_ = 0;
    uint64_t emitted_output_samples_ = 0;
    uint64_t generated_output_samples_ = 0;
    std::vector<int16_t> pending_output_;

#if defined(ESP_PLATFORM)
    void* handle_ = nullptr;
    bool ConvertDeviceBlock(const int16_t* input, size_t samples);
#else
    uint64_t host_input_base_ = 0;
    std::vector<int16_t> host_input_;
    void GenerateHostOutput();
#endif

    uint64_t ExpectedOutputSamples() const;
    void ReleaseExpected(std::vector<int16_t>* output);
};

#endif  // RECORDER_RATE_CONVERTER_H_
```

- [ ] **Step 4: Implement exact streaming behavior**

Create `recorder_rate_converter.cc`. The implementation must use these rules:

```cpp
#include "recorder_rate_converter.h"

#include <algorithm>

#if defined(ESP_PLATFORM)
#include "esp_ae_rate_cvt.h"
#endif

uint64_t RecorderRateConverter::ExpectedOutputSamples() const {
    return total_input_samples_ * static_cast<uint64_t>(destination_rate_) /
           static_cast<uint64_t>(source_rate_);
}

void RecorderRateConverter::ReleaseExpected(std::vector<int16_t>* output) {
    const uint64_t target = ExpectedOutputSamples();
    const uint64_t wanted = target - emitted_output_samples_;
    const size_t count = static_cast<size_t>(
        std::min<uint64_t>(wanted, pending_output_.size()));
    output->insert(output->end(), pending_output_.begin(), pending_output_.begin() + count);
    pending_output_.erase(pending_output_.begin(), pending_output_.begin() + count);
    emitted_output_samples_ += count;
}
```

On ESP-IDF, construct `esp_ae_rate_cvt` with mono, 16-bit, complexity 2 and `ESP_AE_RATE_CVT_PERF_TYPE_MEMORY`. `ConvertDeviceBlock()` must call `esp_ae_rate_cvt_get_max_out_sample_num()`, allocate that many samples, call `esp_ae_rate_cvt_process()`, append the actual output to `pending_output_`, and increment `generated_output_samples_`.

On the host, generate deterministic nearest-neighbor samples by global stream position so output does not change at chunk boundaries:

```cpp
void RecorderRateConverter::GenerateHostOutput() {
    const uint64_t target = ExpectedOutputSamples();
    while (generated_output_samples_ < target) {
        const uint64_t source_index = generated_output_samples_ *
            static_cast<uint64_t>(source_rate_) /
            static_cast<uint64_t>(destination_rate_);
        const size_t local = static_cast<size_t>(source_index - host_input_base_);
        pending_output_.push_back(host_input_.at(local));
        ++generated_output_samples_;
    }

    const uint64_t next_source = generated_output_samples_ *
        static_cast<uint64_t>(source_rate_) /
        static_cast<uint64_t>(destination_rate_);
    const size_t discard = static_cast<size_t>(
        std::min<uint64_t>(next_source - host_input_base_, host_input_.size()));
    host_input_.erase(host_input_.begin(), host_input_.begin() + discard);
    host_input_base_ += discard;
}
```

`Process()` must reject null output, invalid state, or calls after flush; append input to the host buffer; increment `total_input_samples_`; run `GenerateHostOutput()` or `ConvertDeviceBlock()`; then call `ReleaseExpected()`. `Flush()` must make exactly `ExpectedOutputSamples()` available. On ESP-IDF, if the library is short because of filter latency, feed up to eight 256-sample zero blocks without incrementing `total_input_samples_`, then return false unless the exact target count can be emitted. `Reset()` must call `esp_ae_rate_cvt_reset()` on device and clear every counter and buffer.

- [ ] **Step 5: Add the source to firmware CMake**

Add immediately before the existing reducer source:

```cmake
list(APPEND SOURCES "apps/recorder/recorder_rate_converter.cc")
list(APPEND SOURCES "apps/recorder/recorder_noise_reducer.cc")
```

- [ ] **Step 6: Run GREEN and commit**

Run the compile command from Step 2 followed by:

```bash
/tmp/recorder_rate_converter_test
```

Expected: `recorder_rate_converter_test passed`.

Commit:

```bash
git add main/CMakeLists.txt \
  main/apps/recorder/recorder_rate_converter.h \
  main/apps/recorder/recorder_rate_converter.cc \
  main/apps/recorder/recorder_rate_converter_test.cc
git commit -m "feat(recorder): add streaming sample-rate conversion"
```

### Task 2: Official ESP-SR low-memory noise suppression

**Files:**
- Modify: `main/apps/recorder/recorder_noise_reducer.h`
- Modify: `main/apps/recorder/recorder_noise_reducer.cc`
- Modify: `main/apps/recorder/recorder_noise_reducer_test.cc`

- [ ] **Step 1: Replace the old test with a failing frame-pipeline test**

Replace the entire test file. Use an injectable test processor so the host test proves every complete 160-sample frame goes through the NS boundary:

```cpp
#include "recorder_noise_reducer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct FakeNs {
    int calls = 0;
    static void Process(void* context, int16_t* input, int16_t* output) {
        auto* self = static_cast<FakeNs*>(context);
        ++self->calls;
        for (size_t i = 0; i < 160; ++i) {
            output[i] = static_cast<int16_t>(input[i] / 2);
        }
    }
};

void TestEveryFrameUsesNoiseSuppressor() {
    FakeNs fake;
    RecorderNoiseReducer reducer(24000, FakeNs::Process, &fake);
    std::vector<int16_t> input(2400, 100);
    std::vector<int16_t> output;
    Check(reducer.Process(input, &output), "process succeeds");
    Check(reducer.Flush(&output), "flush succeeds");
    Check(output.size() == 1600, "24k input becomes 16k output");
    Check(fake.calls == 10, "all ten 10ms frames use NS");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 300; }),
          "NS output receives fixed post gain");
}

void Passthrough(void*, int16_t* input, int16_t* output) {
    std::copy(input, input + 160, output);
}

void TestChunkedTailKeepsExactDuration() {
    FakeNs fake;
    RecorderNoiseReducer reducer(24000, FakeNs::Process, &fake);
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(123, 100), &output),
          "first uneven block processes");
    Check(reducer.Process(std::vector<int16_t>(456, 100), &output),
          "second uneven block processes");
    Check(reducer.Flush(&output), "uneven stream flushes");
    Check(output.size() == 579 * 16000 / 24000,
          "tail preserves converted duration");
    Check(fake.calls == 3, "partial tail is zero-padded through NS");
}

void TestLimiterCapsPostGain() {
    RecorderNoiseReducer reducer(24000, Passthrough, nullptr);
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(240, 6000), &output),
          "loud frame processes");
    Check(reducer.Flush(&output), "loud frame flushes");
    Check(output.size() == 160, "one output frame produced");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 30000; }),
          "post gain is limited to 30000");
}

void TestNoNsFallbackDoesNotUseDeletedGate() {
    RecorderNoiseReducer reducer(24000);
    Check(!reducer.noise_reduction_enabled(), "host default has no NS backend");
    std::vector<int16_t> output;
    Check(reducer.Process(std::vector<int16_t>(240, 20), &output),
          "fallback frame processes");
    Check(reducer.Flush(&output), "fallback frame flushes");
    Check(std::all_of(output.begin(), output.end(),
                      [](int16_t sample) { return sample == 120; }),
          "small samples are gained, not hard-gated to zero");
}
}  // namespace

int main() {
    TestEveryFrameUsesNoiseSuppressor();
    TestChunkedTailKeepsExactDuration();
    TestLimiterCapsPostGain();
    TestNoNsFallbackDoesNotUseDeletedGate();
    std::puts("recorder_noise_reducer_test passed");
    return 0;
}
```

- [ ] **Step 2: Run the reducer test and verify RED**

Run:

```bash
cd /Users/kanayama/xiaozhi-kb
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_noise_reducer_test.cc \
  main/apps/recorder/recorder_noise_reducer.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/recorder_noise_reducer_test
```

Expected: compilation fails because the old in-place API has no injectable frame processor or output buffer.

- [ ] **Step 3: Replace the reducer interface**

Use this exact public API in `recorder_noise_reducer.h`:

```cpp
class RecorderNoiseReducer {
public:
    using FrameProcessor = void (*)(void* context, int16_t* input, int16_t* output);

    explicit RecorderNoiseReducer(int input_sample_rate,
                                  FrameProcessor processor = nullptr,
                                  void* processor_context = nullptr);
    ~RecorderNoiseReducer();
    RecorderNoiseReducer(const RecorderNoiseReducer&) = delete;
    RecorderNoiseReducer& operator=(const RecorderNoiseReducer&) = delete;

    bool Process(const std::vector<int16_t>& input, std::vector<int16_t>* output);
    bool Flush(std::vector<int16_t>* output);
    bool Reset();
    bool valid() const { return rate_converter_.valid(); }
    bool noise_reduction_enabled() const { return frame_processor_ != nullptr; }
    int output_sample_rate() const { return 16000; }

private:
    RecorderRateConverter rate_converter_;
    FrameProcessor frame_processor_ = nullptr;
    void* processor_context_ = nullptr;
    bool owns_processor_context_ = false;
    std::vector<int16_t> pending_16k_;

    void ProcessCompleteFrames(std::vector<int16_t>* output);
    void ProcessOneFrame(int16_t* input, int16_t* output);
    static int16_t ApplyGainAndLimit(int16_t sample);
};
```

Include `recorder_rate_converter.h` in this header. Remove `agc_handle_`, the in-place `Process`, `ProcessWithEspSr`, `ApplyFallbackEnhancement`, and the absolute-value noise gate.

- [ ] **Step 4: Implement the official ESP-SR backend**

In the device constructor, log internal heap before and after initialization, call only `ns_create(10)`, and install a frame callback only when the returned handle is non-null:

```cpp
#if defined(ESP_PLATFORM)
const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
ESP_LOGI(kTag, "NS heap before: free=%u largest=%u",
         static_cast<unsigned>(heap_caps_get_free_size(caps)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
if (frame_processor_ == nullptr) {
    processor_context_ = ns_create(10);
    owns_processor_context_ = processor_context_ != nullptr;
    if (processor_context_ != nullptr) {
        frame_processor_ = [](void* context, int16_t* input, int16_t* output) {
            ns_process(context, input, output);
        };
    }
}
ESP_LOGI(kTag, "NS heap after: free=%u largest=%u",
         static_cast<unsigned>(heap_caps_get_free_size(caps)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(caps)));
ESP_LOGI(kTag, "ESP-SR NS %s, input=%d Hz output=16000 Hz frame=160",
         noise_reduction_enabled() ? "enabled" : "unavailable",
         input_sample_rate);
#endif
```

Implement streaming and tail processing exactly as follows; `ProcessCompleteFrames()` repeatedly removes the first 160 samples, calls `ProcessOneFrame()`, and appends all 160 processed samples:

```cpp
bool RecorderNoiseReducer::Process(const std::vector<int16_t>& input,
                                   std::vector<int16_t>* output) {
    if (output == nullptr || !valid()) {
        return false;
    }
    std::vector<int16_t> converted;
    if (!rate_converter_.Process(input, &converted)) {
        return false;
    }
    pending_16k_.insert(pending_16k_.end(), converted.begin(), converted.end());
    ProcessCompleteFrames(output);
    return true;
}

bool RecorderNoiseReducer::Flush(std::vector<int16_t>* output) {
    if (output == nullptr || !valid()) {
        return false;
    }
    std::vector<int16_t> converted;
    if (!rate_converter_.Flush(&converted)) {
        return false;
    }
    pending_16k_.insert(pending_16k_.end(), converted.begin(), converted.end());
    ProcessCompleteFrames(output);
    if (!pending_16k_.empty()) {
        const size_t valid_samples = pending_16k_.size();
        int16_t input[160] = {};
        int16_t processed[160] = {};
        std::copy(pending_16k_.begin(), pending_16k_.end(), input);
        ProcessOneFrame(input, processed);
        output->insert(output->end(), processed, processed + valid_samples);
        pending_16k_.clear();
    }
    return true;
}

void RecorderNoiseReducer::ProcessOneFrame(int16_t* input, int16_t* output) {
    if (frame_processor_ != nullptr) {
        frame_processor_(processor_context_, input, output);
    } else {
        std::copy(input, input + 160, output);
    }
    std::transform(output, output + 160, output, ApplyGainAndLimit);
}

int16_t RecorderNoiseReducer::ApplyGainAndLimit(int16_t sample) {
    return static_cast<int16_t>(std::clamp(static_cast<int>(sample) * 6,
                                           -30000,
                                           30000));
}
```

The destructor calls `ns_destroy()` only when `owns_processor_context_` is true. `Reset()` clears `pending_16k_` and calls `rate_converter_.Reset()`. Do not include or call `esp_agc.h`, `ns_pro_create()`, or the old noise gate.

- [ ] **Step 5: Run GREEN and verify forbidden symbols are gone from source**

Run the command from Step 2, then:

```bash
/tmp/recorder_noise_reducer_test
rg -n "ns_pro_create|esp_agc|kFallbackNoiseGate|ApplyFallbackEnhancement" \
  main/apps/recorder/recorder_noise_reducer.*
```

Expected: test exits 0; `rg` prints no matches.

- [ ] **Step 6: Commit**

```bash
git add main/apps/recorder/recorder_noise_reducer.h \
  main/apps/recorder/recorder_noise_reducer.cc \
  main/apps/recorder/recorder_noise_reducer_test.cc
git commit -m "feat(recorder): process audio with low-memory ESP-SR NS"
```

### Task 3: Accept and play 16 kHz recorder WAV files

**Files:**
- Modify: `main/apps/recorder/recorder_wav_file.h`
- Modify: `main/apps/recorder/recorder_wav_file.cc`
- Modify: `main/apps/recorder/recorder_playback_menu_test.cc`
- Modify: `main/apps/recorder/recorder_app.cc:230-286`

- [ ] **Step 1: Add failing WAV compatibility cases**

Replace `RecorderWavMatchesCodec` assertions with:

```cpp
Check(RecorderWavCanPlay(
          RecorderWavInfo{24000, 1, 16, 44, 48000}, 24000, 1),
      "native 24k wav is playable");
Check(RecorderWavCanPlay(
          RecorderWavInfo{16000, 1, 16, 44, 32000}, 24000, 1),
      "enhanced 16k wav is playable through resampler");
Check(!RecorderWavCanPlay(
          RecorderWavInfo{22050, 1, 16, 44, 44100}, 24000, 1),
      "unsupported sample rate is rejected");
Check(!RecorderWavCanPlay(
          RecorderWavInfo{16000, 2, 16, 44, 64000}, 24000, 1),
      "channel mismatch is rejected");
```

- [ ] **Step 2: Run playback tests and verify RED**

```bash
cd /Users/kanayama/xiaozhi-kb
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test
```

Expected: compilation fails because `RecorderWavCanPlay` is not defined.

- [ ] **Step 3: Implement explicit playback compatibility**

Replace the old function declaration and implementation with:

```cpp
bool RecorderWavCanPlay(const RecorderWavInfo& info,
                        int output_sample_rate,
                        int output_channels) {
    const bool supported_rate =
        info.sample_rate == 16000 ||
        info.sample_rate == static_cast<uint32_t>(output_sample_rate);
    return supported_rate &&
           info.channels == static_cast<uint16_t>(output_channels) &&
           info.bits_per_sample == 16 &&
           info.data_offset == 44 &&
           info.data_bytes > 0;
}
```

- [ ] **Step 4: Resample playback only when required**

In `PlayWavFile()`, use `RecorderWavCanPlay`, construct `RecorderRateConverter playback_rate(info.sample_rate, codec.output_sample_rate())`, and replace direct `codec.OutputData(playback_buf)` with:

```cpp
if (!playback_rate.valid()) {
    ESP_LOGE(TAG, "播放重采样器初始化失败: %u -> %d",
             static_cast<unsigned>(info.sample_rate),
             codec.output_sample_rate());
    fclose(playback);
    return false;
}

std::vector<int16_t> converted;
if (!playback_rate.Process(playback_buf, &converted)) {
    ok = false;
    break;
}
if (!converted.empty()) {
    codec.OutputData(converted);
}
```

After the read loop, when not interrupted, call `playback_rate.Flush(&converted)` and send the returned tail. Treat converter failure as playback failure. Same-rate 24 kHz files remain byte-for-byte passthrough through the converter.

- [ ] **Step 5: Run GREEN and commit**

Run the playback compile command and `/tmp/recorder_playback_menu_test`.

Expected: `recorder_playback_menu_test passed`.

Commit:

```bash
git add main/apps/recorder/recorder_wav_file.h \
  main/apps/recorder/recorder_wav_file.cc \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_app.cc
git commit -m "feat(recorder): play 16 and 24 khz wav files"
```

### Task 4: Write enhanced 16 kHz PCM and flush every stop path

**Files:**
- Modify: `main/apps/recorder/recorder_app.cc:316-440`

- [ ] **Step 1: Add focused write helpers**

Inside the anonymous namespace add:

```cpp
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
```

Keep file closing in the caller so failures still produce a recoverable header whenever possible.

- [ ] **Step 2: Reset and write a 16 kHz header at recording start**

Before opening a new file, call `noise_reducer.Reset()`. After open, write:

```cpp
WriteWavHeader(f, noise_reducer.output_sample_rate(), 1, 16, 0);
```

If `noise_reducer.valid()` is false, show `NS FAILED` and do not start recording. If the reducer is valid but `noise_reduction_enabled()` is false, show `NS OFF` in the subtitle and log that the current recording will be resampled without suppression.

- [ ] **Step 3: Replace in-place processing with output processing**

Add `bool recording_failed = false;` beside `data_bytes`, and reset it to false whenever a new WAV is opened. Replace the current call and write block with:

```cpp
std::vector<int16_t> processed;
if (!noise_reducer.Process(buf, &processed) ||
    !AppendPcm(f, processed, &data_bytes)) {
    ESP_LOGE(TAG, "录音 DSP/写入失败: %s", cur_path);
    s_recording = false;
    recording_failed = true;
}
```

Calculate elapsed time using `noise_reducer.output_sample_rate()` and one output channel.

- [ ] **Step 4: Flush normal stop and exit stop**

Both the `s_exit` branch and the normal `!s_recording && f != nullptr` branch must call `FinishRecording()` before `fclose()`. Log the failure and show `SAVE FAILED` when it returns false. Remove every remaining call that writes a WAV header with the codec's 24 kHz input rate.

The normal stop branch must compute `const bool saved_ok = FinishRecording(f, &noise_reducer, &data_bytes) && !recording_failed;`, always close the file, and only show `SAVED`/perform the serial dump when `saved_ok` is true.

- [ ] **Step 5: Surface actual NS state**

After constructing the reducer, log:

```cpp
ESP_LOGI(TAG, "recorder DSP: input=%d output=%d ns=%s",
         sample_rate,
         noise_reducer.output_sample_rate(),
         noise_reducer.noise_reduction_enabled() ? "enabled" : "unavailable");
```

The idle subtitle must say `NS READY / left: start` when the official handle exists, and `NS OFF / left: start` otherwise.

- [ ] **Step 6: Run all host tests and build ESP32-C6 firmware**

Run:

```bash
cd /Users/kanayama/xiaozhi-kb
/tmp/recorder_rate_converter_test
/tmp/recorder_noise_reducer_test
/tmp/recorder_playback_menu_test
source /Users/kanayama/esp/esp-idf/export.sh
idf.py build
```

Expected: all three tests exit 0 and `idf.py build` ends with `Project build complete`.

- [ ] **Step 7: Prove the firmware links the intended official API**

Run:

```bash
cd /Users/kanayama/xiaozhi-kb
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
rg -n "ns_pro_create|kFallbackNoiseGate|ApplyFallbackEnhancement" \
  main/apps/recorder main/CMakeLists.txt
```

Expected: `nm` prints `U ns_create`; neither command shows a recorder reference to `ns_pro_create` or the deleted gate.

- [ ] **Step 8: Commit**

```bash
git add main/apps/recorder/recorder_app.cc
git commit -m "feat(recorder): save ESP-SR enhanced 16 khz audio"
```

### Task 5: Documentation and real-device acceptance

**Files:**
- Modify: `README.md:155-166`
- Modify: `docs/recorder-design-guardrails.md:45-75`

- [ ] **Step 1: Update documentation to match the implemented chain**

Update README recorder bullets to state:

```markdown
- 新录音保存为 WAV（单声道 / 16bit / 16000Hz）；播放器同时兼容旧 24000Hz 和新 16000Hz 文件。
- 录音链路把 ES7210 的 24000Hz PCM 用小智同款 `esp_ae_rate_cvt` 转为 16000Hz，再按 10ms 帧调用 ESP-SR `ns_create` / `ns_process`，最后进行固定增益和限幅。
```

Replace the guardrail that mandates the fallback gate with the verified distinction: full AFE and `ns_pro_create()` remain unsupported/unsafe on C6, while the low-memory `ns_create(10)` path is allowed only when boot logs and a real recording prove it initialized and stayed stable.

- [ ] **Step 2: Build and flash the complete firmware**

With the board in download mode, run:

```bash
cd /Users/kanayama/xiaozhi-kb
source /Users/kanayama/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

After flashing, physically power-cycle the board without holding BOOT if automatic reset leaves the C6 in ROM download mode.

- [ ] **Step 3: Capture passive boot evidence**

Start this 30-second passive capture, then physically cold-boot the board while it is running. It sets DTR/RTS before opening the port and never resets the target:

```bash
/tmp/ser-venv/bin/python - <<'PY'
import time
import serial

port = serial.Serial()
port.port = "/dev/cu.usbmodem1101"
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
deadline = time.monotonic() + 30
with open("/tmp/recorder-ns-boot.log", "wb") as output:
    while time.monotonic() < deadline:
        data = port.read(4096)
        if data:
            output.write(data)
            output.flush()
port.close()
PY
rg -n "NS heap|ESP-SR NS|recorder DSP|Guru Meditation|Rebooting|spi_hal_setup_trans" \
  /tmp/recorder-ns-boot.log
```

Capture at least 15 seconds after the first boot banner. Required log evidence:

```text
NS heap before: free=... largest=...
NS heap after: free=... largest=...
ESP-SR NS enabled, input=24000 Hz output=16000 Hz frame=160
recorder DSP: input=24000 output=16000 ns=enabled
```

Reject the build if logs contain `Guru Meditation`, `Rebooting`, `spi_hal_setup_trans`, `NS unavailable`, or repeated boot banners.

- [ ] **Step 4: Record the fixed acoustic sample**

Run a passive serial capture, then in Recorder mode capture at least 2 seconds of the current environment, speak normally, and leave at least 2 seconds of the same environment before stopping:

```bash
/tmp/ser-venv/bin/python - <<'PY'
import time
import serial

port = serial.Serial()
port.port = "/dev/cu.usbmodem1101"
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
deadline = time.monotonic() + 120
with open("/tmp/recorder-ns-session.log", "wb") as output:
    while time.monotonic() < deadline:
        data = port.read(4096)
        if data:
            output.write(data)
            output.flush()
            if b"<<<WAV_END>>>" in data:
                break
port.close()
PY
```

Confirm on-device playback completes without rebooting, then decode the last complete serial payload:

```bash
/tmp/ser-venv/bin/python - <<'PY'
import base64
import re

data = open("/tmp/recorder-ns-session.log", "rb").read()
matches = list(re.finditer(
    rb"<<<WAV_BEGIN [^>]+>>>\s*(.*?)\s*<<<WAV_END>>>",
    data,
    re.S,
))
if not matches:
    raise SystemExit("no complete WAV payload")
payload = re.sub(rb"\s+", b"", matches[-1].group(1))
open("/tmp/recorder-ns.wav", "wb").write(base64.b64decode(payload))
PY
```

- [ ] **Step 5: Verify WAV format and noise behavior**

Decode the serial base64 payload, then run:

```bash
ffprobe -v error -show_entries stream=sample_rate,channels,bits_per_sample \
  -of default=noprint_wrappers=1 /tmp/recorder-ns.wav
```

Expected: `sample_rate=16000`, `channels=1`, `bits_per_sample=16`.

Listen to the full clip and inspect the leading/trailing environment-only regions. Acceptance requires clearly lower steady environment noise than the previous firmware while speech remains intelligible, without obvious pumping, clipped peaks, or missing syllables.

- [ ] **Step 6: Verify old and new playback compatibility**

From the device's PLAY menu, play one old 24 kHz recording and the new 16 kHz recording. Both must reach `PLAY DONE`; the board must not reboot.

- [ ] **Step 7: Run final regression checks and commit documentation**

Run all three host tests, `idf.py build`, `git diff --check`, and `git status --short`. Commit only after the real-device result is recorded accurately:

```bash
git add README.md docs/recorder-design-guardrails.md
git commit -m "docs: record ESP32-C6 recorder NS validation"
```

Expected final state: tests and build pass, the working tree is clean, device logs prove `ns_create` is active, a new 16 kHz WAV is audibly cleaner, and both WAV rates play successfully.
