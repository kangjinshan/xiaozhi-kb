# Recorder Touch Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make recorder mode fully touchscreen-operated, add pause/resume and playback volume keys, and exit through a visible two-second `MENU` hold while preserving SPI2-safe audio and display access.

**Architecture:** A platform-neutral reducer owns recorder control state and validates touch/physical-key events. LVGL translates gestures into queued events only; `recorder_app` consumes them on its task, performs all SD/codec work, and alternates LVGL/SD ownership around bounded recording writes and playback reads.

**Tech Stack:** C++17, ESP-IDF, LVGL 9, ESP LVGL Port, FreeRTOS, BoxAudioCodec, host-compiled assertion tests

---

## File Structure

- Create `main/apps/recorder/recorder_control_state.h`: platform-neutral states, input events, actions, reducer, and wrap-safe MENU hold helper.
- Create `main/apps/recorder/recorder_control_state.cc`: state transition, physical-key filtering, volume clamp, and hold-duration logic.
- Create `main/apps/recorder/recorder_control_state_test.cc`: host coverage for touch recording, disabled recording keys, pause/resume, volume bounds, and MENU timing.
- Modify `main/apps/recorder/recorder_display.h`: state-oriented display and complete touch callback API.
- Modify `main/apps/recorder/recorder_display.cc`: visible `MENU`, `REC`, `PLAY`, `STOP`, `PAUSE`, and `RESUME` widgets plus two-second hold detection.
- Modify `main/apps/recorder/recorder_app.cc`: queued request processing, state-dependent physical keys, interactive playback, and SPI2-safe touch recording.
- Modify `main/apps/recorder/recorder_rate_converter_test.cc`: device-tail regression contract is documented and host stream invariance remains covered.
- Modify `main/CMakeLists.txt`: link the control-state implementation into firmware.
- Modify `README.md` and `docs/recorder-design-guardrails.md`: document controls and exact verification commands.

### Task 1: Platform-neutral recorder control reducer

**Files:**
- Create: `main/apps/recorder/recorder_control_state.h`
- Create: `main/apps/recorder/recorder_control_state.cc`
- Test: `main/apps/recorder/recorder_control_state_test.cc`

- [ ] **Step 1: Write the failing state tests**

Create a standalone test whose `main()` calls these complete cases:

```cpp
void TestTouchRecordAndStop() {
    RecorderControlState state;
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchRecord) ==
              RecorderControlAction::kStartRecording,
          "REC starts recording");
    Check(state.mode == RecorderControlMode::kRecording, "recording state entered");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchStop) ==
              RecorderControlAction::kStopRecording,
          "STOP stops recording");
    Check(state.mode == RecorderControlMode::kIdle, "idle state restored");
}

void TestRecordingIgnoresPhysicalKeys() {
    RecorderControlState state{RecorderControlMode::kRecording, 70};
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kNone,
          "left key ignored while recording");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight) ==
              RecorderControlAction::kNone,
          "right key ignored while recording");
    Check(state.volume == 70, "ignored keys keep volume");
}

void TestPauseResumeAndVolumeBounds() {
    RecorderControlState state{RecorderControlMode::kPlaying, 95};
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchPauseResume) ==
              RecorderControlAction::kPausePlayback,
          "PAUSE pauses");
    Check(state.mode == RecorderControlMode::kPaused, "paused state entered");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalLeft) ==
              RecorderControlAction::kVolumeChanged,
          "left key raises volume while paused");
    Check(state.volume == 100, "volume clamps at 100");
    Check(RecorderControlReduce(&state, RecorderControlEvent::kTouchPauseResume) ==
              RecorderControlAction::kResumePlayback,
          "RESUME resumes");
    Check(state.mode == RecorderControlMode::kPlaying, "playing state restored");
    for (int i = 0; i < 11; ++i) RecorderControlReduce(&state, RecorderControlEvent::kPhysicalRight);
    Check(state.volume == 0, "volume clamps at zero");
}

void TestMenuHoldTiming() {
    Check(!RecorderMenuHoldReached(1000, 2999), "1999 ms does not exit");
    Check(RecorderMenuHoldReached(1000, 3000), "2000 ms exits");
    Check(RecorderMenuHoldReached(0xFFFFFF00u, 0x000006D0u), "hold timing survives tick wrap");
}
```

- [ ] **Step 2: Run the new test and verify it fails**

```bash
c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/recorder \
  main/apps/recorder/recorder_control_state_test.cc \
  main/apps/recorder/recorder_control_state.cc \
  -o /tmp/recorder_control_state_test
```

Expected: compilation fails because `recorder_control_state.h/.cc` do not exist.

- [ ] **Step 3: Implement the minimal reducer**

Define the exact public interface:

```cpp
enum class RecorderControlMode { kIdle, kRecording, kPlaying, kPaused };
enum class RecorderControlEvent {
    kTouchRecord, kTouchStop, kTouchPlay, kTouchPauseResume,
    kPhysicalLeft, kPhysicalRight, kPlaybackSelected, kPlaybackFinished,
    kExitRequested,
};
enum class RecorderControlAction {
    kNone, kStartRecording, kStopRecording, kOpenPlaybackMenu,
    kPausePlayback, kResumePlayback, kVolumeChanged, kStartPlayback, kExit,
};
struct RecorderControlState {
    RecorderControlMode mode = RecorderControlMode::kIdle;
    int volume = 70;
};
RecorderControlAction RecorderControlReduce(RecorderControlState* state,
                                             RecorderControlEvent event);
bool RecorderMenuHoldReached(uint32_t pressed_tick, uint32_t now_tick);
```

Implement `RecorderControlReduce()` with a `switch`: only idle accepts record/play/file selection, only recording accepts STOP, only playing/paused accepts pause or `±10`, and every state accepts exit. Use `std::min(100, volume + 10)` and `std::max(0, volume - 10)`. Implement the timer as `static_cast<uint32_t>(now_tick - pressed_tick) >= 2000`.

- [ ] **Step 4: Run the reducer test and verify it passes**

Run the compile command from Step 2 followed by `/tmp/recorder_control_state_test`.
Expected: `recorder_control_state_test passed`.

- [ ] **Step 5: Commit the reducer**

```bash
git add main/apps/recorder/recorder_control_state.h main/apps/recorder/recorder_control_state.cc \
        main/apps/recorder/recorder_control_state_test.cc
git commit -m "feat(recorder): add touch control state reducer"
```

### Task 2: Stateful LVGL recorder controls

**Files:**
- Modify: `main/apps/recorder/recorder_display.h`
- Modify: `main/apps/recorder/recorder_display.cc`

- [ ] **Step 1: Replace the narrow callback API with complete touch callbacks**

```cpp
enum class RecorderDisplayState { kIdle, kRecording, kPlaying, kPaused };
struct RecorderDisplayCallbacks {
    RecorderDisplayCallback record = nullptr;
    RecorderDisplayCallback stop = nullptr;
    RecorderDisplayCallback open_menu = nullptr;
    RecorderDisplayCallback pause_resume = nullptr;
    RecorderDisplayCallback exit = nullptr;
    RecorderDisplayFileCallback play_file = nullptr;
};
void RecorderDisplaySetCallbacks(const RecorderDisplayCallbacks& callbacks, void* user_data);
void RecorderDisplaySetState(RecorderDisplayState state, int volume);
```

Remove `RecorderDisplaySetPlayMenuVisible()`; visibility becomes a single state-rendering operation.

- [ ] **Step 2: Add widgets and callbacks with exact event behavior**

Keep pointers for global MENU, REC, PLAY, centered action, and labels. Route clicks to callbacks. Implement MENU as:

```cpp
void OnMenuEvent(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_menu_pressed_tick = lv_tick_get();
        s_menu_hold_fired = false;
    } else if (code == LV_EVENT_PRESSING && !s_menu_hold_fired &&
               RecorderMenuHoldReached(s_menu_pressed_tick, lv_tick_get())) {
        s_menu_hold_fired = true;
        if (s_callbacks.exit != nullptr) s_callbacks.exit(s_callback_user_data);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_menu_hold_fired = false;
    }
}
```

Create `MENU` at `LV_ALIGN_TOP_LEFT`, size `100 x 48`. In the recordings overlay reserve the first 108 header pixels, keep `BACK` at right, and move global MENU foreground after showing the overlay.

- [ ] **Step 3: Render every state from one function**

`RecorderDisplaySetState()` locks LVGL and maps: idle shows REC/PLAY; recording shows centered STOP; playing shows centered PAUSE and `VOL n`; paused shows centered RESUME and `VOL n`. Place idle buttons at center x `-100/+100`, both `170 x 58`; action at center y=70, `210 x 62`; MENU remains visible.

- [ ] **Step 4: Build firmware to catch LVGL/API errors**

Run `source ~/esp/esp-idf/export.sh && idf.py build`.
Expected: build fails only until Task 3 updates app call sites; there must be no unrelated LVGL symbol/type error.

- [ ] **Step 5: Commit display and app API migration together in Task 3**

Keep this intentionally uncompilable intermediate change uncommitted until Task 3.

### Task 3: Queue requests and apply state-dependent controls

**Files:**
- Modify: `main/apps/recorder/recorder_app.cc`
- Modify: `main/CMakeLists.txt`
- Modify: `main/apps/recorder/recorder_display.h`
- Modify: `main/apps/recorder/recorder_display.cc`

- [ ] **Step 1: Link the control reducer**

Add `list(APPEND SOURCES "apps/recorder/recorder_control_state.cc")` beside recorder sources.

- [ ] **Step 2: Replace callback-owned operational flags with a request queue**

```cpp
struct RecorderRequest { RecorderControlEvent event; std::string path; };
std::mutex s_request_mutex;
std::deque<RecorderRequest> s_requests;
void PublishRequest(RecorderControlEvent event, const char* path = nullptr) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    s_requests.push_back({event, path != nullptr ? path : ""});
}
bool TakeRequest(RecorderRequest* request) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    if (s_requests.empty() || request == nullptr) return false;
    *request = std::move(s_requests.front());
    s_requests.pop_front();
    return true;
}
```

Touch callbacks only publish. Left/right `Button::OnClick()` publish `kPhysicalLeft/kPhysicalRight`. Delete left-key record toggling and BOOT long-press exit registration.

- [ ] **Step 3: Apply requests only on the recorder task**

Create `RecorderControlState control{RecorderControlMode::kIdle, codec.output_volume()}`. For each dequeued request call the reducer, then execute its action. Convert display state with a total `switch`. For `kVolumeChanged`, call `codec.SetOutputVolume(control.volume)` and refresh display. For `kExit`, set a task-owned exit flag. No callback performs SD, WAV, or codec work.

- [ ] **Step 4: Make recording alternate LVGL and SD ownership**

At start pause LVGL for file creation/header, set recording state, then resume. For every microphone block, keep LVGL running during input and DSP, pause before `AppendPcm`, update elapsed text while paused, resume immediately, then consume requests. STOP reaches the existing reducer flush/header rewrite/close path; MENU finalizes before reboot.

- [ ] **Step 5: Run host tests and build firmware**

Run the new control test, rate-converter test, then `source ~/esp/esp-idf/export.sh && idf.py build`.
Expected: host tests pass and ESP-IDF reports `Project build complete`.

- [ ] **Step 6: Commit the complete control UI**

```bash
git add main/CMakeLists.txt main/apps/recorder/recorder_app.cc \
        main/apps/recorder/recorder_display.h main/apps/recorder/recorder_display.cc
git commit -m "feat(recorder): add touchscreen recording controls"
```

### Task 4: Interactive SPI2-safe playback

**Files:**
- Modify: `main/apps/recorder/recorder_app.cc`
- Modify: `main/apps/recorder/recorder_rate_converter.h`
- Modify: `main/apps/recorder/recorder_rate_converter.cc`
- Modify: `main/apps/recorder/recorder_rate_converter_test.cc`

- [ ] **Step 1: Preserve the device rate-converter maximum capacity**

Keep the ESP-only `uint32_t device_output_capacity_ = 0;` and `device_output_capacity_ = std::max(device_output_capacity_, required_capacity);`. This prevents a short final 16→24 kHz block from undersizing samples emitted from the converter cache.

- [ ] **Step 2: Bound each source read at 4096 samples**

Define `constexpr size_t kPlaybackReadSamples = 4096`. Allocate that many source frames. Before each `fread`, pause display; immediately afterward resume before conversion and `OutputData`.

- [ ] **Step 3: Service pause, resume, volume, and exit during playback**

After each output block drain requests. While paused, read no SD and output no PCM; keep LVGL running, drain requests, and delay 20 ms. Resume without resetting/flushing the converter. Exit interrupts.

- [ ] **Step 4: Return with one balanced LVGL pause for file close**

On every return leave display paused once. Flush audio only after natural EOF while LVGL runs. Then pause, disable output, close, and return done/failed/interrupted. Caller updates result or exits, then resumes once.

- [ ] **Step 5: Run all recorder host tests and firmware build**

Fresh-compile and run control-state, rate-converter, noise-reducer, and playback-menu tests, then run `idf.py build`.
Expected: four host tests pass and firmware builds.

- [ ] **Step 6: Commit playback and capacity fix**

```bash
git add main/apps/recorder/recorder_app.cc main/apps/recorder/recorder_rate_converter.h \
        main/apps/recorder/recorder_rate_converter.cc main/apps/recorder/recorder_rate_converter_test.cc
git commit -m "feat(recorder): add pause and playback volume controls"
```

### Task 5: Documentation, static verification, and device acceptance

**Files:**
- Modify: `README.md`
- Modify: `docs/recorder-design-guardrails.md`

- [ ] **Step 1: Document the final controls**

```markdown
- 空闲时点屏幕 `REC` 开始录音，录音时点 `STOP` 保存；录音期间实体键禁用。
- 点 `PLAY` 选择录音；播放时点 `PAUSE`/`RESUME`，左键音量 +10，右键音量 -10（0–100）。
- 在任意录音界面持续按住左上角 `MENU` 2 秒返回应用选择界面；短按不会退出。
```

Add the control test command and 16/24 kHz, pause/resume, volume, MENU, and crash-log device checklist.

- [ ] **Step 2: Verify source and linked NS symbols**

```bash
rg -n 'ns_pro_create|OnLongPress|toggle recording' main/apps/recorder
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
```

Expected: no obsolete recorder long-press/toggle or `ns_pro_create`; linked output contains `U ns_create` only.

- [ ] **Step 3: Run the complete fresh verification suite**

Recompile/run all four host tests, then `idf.py build`. Expected: every command exits 0.

- [ ] **Step 4: Flash and monitor the ESP32-C6**

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor
```

Expected logs include `ESP-SR NS enabled` and `input=24000 output=16000 ns=enabled`, with no heap assertion, `Guru Meditation`, `Rebooting`, or `spi_hal_setup_trans` failure.

- [ ] **Step 5: Perform device interaction acceptance**

Verify touchscreen REC/STOP saves 16 kHz mono PCM16; keys do nothing recording; 16/24 kHz files reach `PLAY DONE`; PAUSE silences and RESUME continues; volume stays 0–100; short MENU does nothing; holding MENU two seconds returns to selector.

- [ ] **Step 6: Commit documentation**

```bash
git add README.md docs/recorder-design-guardrails.md
git commit -m "docs: describe recorder touch controls"
```

## Final Self-Review

- Spec coverage: all four UI states, physical-key rules, MENU exit, bounded SPI2 ownership, interruption handling, and verification map to Tasks 1–5.
- Placeholder scan: steps contain concrete types, APIs, commands, expected results, and commit boundaries.
- Type consistency: control mode/event/action/state, display state/callbacks, queue events, and display mapping use the same names throughout.
