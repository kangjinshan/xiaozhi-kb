# Recorder PWR Screen Toggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make each AXP2101 PWR short press toggle the Jinshan AI AMOLED off/on while all assistant work continues.

**Architecture:** Reuse the existing AXP2101 short-press decoder and publish a power event from a PMIC polling task into Recorder's request queue. The pure control reducer owns desired screen state; the Recorder main task applies it through a display API that nests one persistent LVGL pause around SH8601 display-off.

**Tech Stack:** C++17, ESP-IDF 5.5.3 I2C/LCD APIs, FreeRTOS, LVGL 9.5, existing Recorder host tests.

---

### Task 1: Define the pure PWR state transition

**Files:**
- Modify: `main/apps/recorder/recorder_control_state.h`
- Modify: `main/apps/recorder/recorder_control_state.cc`
- Modify: `main/apps/recorder/recorder_control_state_test.cc`

- [x] **Step 1: Write the failing reducer test**

Add `TestPowerKeyTogglesScreenWithoutChangingAssistantMode()` and call it from
`main()`:

```cpp
void TestPowerKeyTogglesScreenWithoutChangingAssistantMode() {
    for (RecorderControlMode mode : {
             RecorderControlMode::kIdle,
             RecorderControlMode::kRecording,
             RecorderControlMode::kPlaying,
             RecorderControlMode::kPaused,
         }) {
        RecorderControlState state{mode, 70};
        state.voice_phase = AgentVoicePhase::kThinking;
        Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalPower) ==
                  RecorderControlAction::kScreenPowerChanged,
              "PWR turns the screen off in every mode");
        Check(!state.screen_on && state.mode == mode && state.volume == 70 &&
                  state.voice_phase == AgentVoicePhase::kThinking,
              "screen off preserves assistant state");
        Check(RecorderControlReduce(&state, RecorderControlEvent::kPhysicalPower) ==
                  RecorderControlAction::kScreenPowerChanged,
              "second PWR press turns the screen on");
        Check(state.screen_on && state.mode == mode,
              "screen on restores only display state");
    }
}
```

- [x] **Step 2: Run the reducer test and verify RED**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_control_state_test.cc \
  main/apps/recorder/recorder_control_state.cc \
  main/apps/recorder/agent_voice_state.cc \
  -o /tmp/recorder_control_state_test && /tmp/recorder_control_state_test
```

Expected: compilation fails because `kPhysicalPower`,
`kScreenPowerChanged`, and `screen_on` do not exist.

- [x] **Step 3: Implement the minimal reducer transition**

Add `kPhysicalPower`, `kScreenPowerChanged`, and `bool screen_on = true`. Before
mode-specific handling in `RecorderControlReduce()` add:

```cpp
if (event == RecorderControlEvent::kPhysicalPower) {
    state->screen_on = !state->screen_on;
    return RecorderControlAction::kScreenPowerChanged;
}
```

- [x] **Step 4: Run the reducer test and verify GREEN**

Run the Step 2 command. Expected:
`recorder_control_state_test passed`.

- [x] **Step 5: Commit the pure transition**

```bash
git add main/apps/recorder/recorder_control_state.h \
  main/apps/recorder/recorder_control_state.cc \
  main/apps/recorder/recorder_control_state_test.cc
git commit -m "feat(recorder): model PWR screen state"
```

### Task 2: Detect PWR and apply SH8601 display power

**Files:**
- Modify: `main/apps/recorder/recorder_app.cc`
- Modify: `main/apps/recorder/recorder_display.h`
- Modify: `main/apps/recorder/recorder_display.cc`

- [x] **Step 1: Re-run the existing PMIC decoder test**

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/keyboard_pmic_power_key_test.cc \
  main/apps/ble_keyboard/keyboard_pmic_power_key.cc \
  -o /tmp/keyboard_pmic_power_key_test && /tmp/keyboard_pmic_power_key_test
```

Expected: exit 0, proving IRQ2 `0x08` is the short-press signal and long-press
bit `0x04` is ignored.

- [x] **Step 2: Enable and poll the AXP2101 short-press IRQ**

In `recorder_app.cc`, include `keyboard_pmic_power_key.h`, add register
constants `0x41`, `0x48`, `0x49`, `0x4A`, and add `ReadReg()`,
`ClearAxp2101IrqStatus()`, and `ConfigureRecorderPowerKey()`. The configuration
must read/OR/write `Axp2101PowerKeyShortPressMask()` and clear stale status.

Add a FreeRTOS task that polls every 25 ms, rate-limits I2C warnings to once per
second, latches one event per asserted status, clears status, and only calls:

```cpp
PublishRequest(RecorderControlEvent::kPhysicalPower);
```

Start it after display callbacks are registered and only when PMIC IRQ
configuration succeeded.

- [x] **Step 3: Add the display transition API**

In `recorder_display.h` declare:

```cpp
esp_err_t RecorderDisplaySetScreenOn(bool screen_on);
```

In `recorder_display.cc`, retain the initialized
`esp_lcd_panel_handle_t` and current screen state. Off must call
`RecorderDisplayPause()` then `esp_lcd_panel_disp_on_off(panel, false)` and keep
that pause held. On must send display-on, invalidate `lv_screen_active()`, then
call `RecorderDisplayResume()`. If a panel command fails, keep the prior applied
state and balance the pause depth.

- [x] **Step 4: Apply the transition from the Recorder main task**

In `ApplyRequest()`, handle `kScreenPowerChanged` before audio side effects:

```cpp
const bool requested_screen_on = control->screen_on;
const esp_err_t err = RecorderDisplaySetScreenOn(requested_screen_on);
if (err != ESP_OK) {
    control->screen_on = !requested_screen_on;
    ESP_LOGW(TAG, "PWR screen transition failed: %s", esp_err_to_name(err));
}
```

This keeps all SPI2 panel commands in the Recorder main task, including while
playback drains requests.

- [x] **Step 5: Build the firmware**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
```

Expected: build exits 0 and only `U ns_create` is present.

- [x] **Step 6: Commit hardware integration**

```bash
git add main/apps/recorder/recorder_app.cc \
  main/apps/recorder/recorder_display.h \
  main/apps/recorder/recorder_display.cc
git commit -m "feat(recorder): toggle AMOLED with PWR"
```

### Task 3: Document and fully verify

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `main/boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md`
- Modify: `docs/recorder-design-guardrails.md`
- Modify: `docs/superpowers/plans/2026-07-12-recorder-pwr-screen-toggle.md`

- [x] **Step 1: Document the PWR contract and SPI2 invariant**

State that Recorder PWR short press toggles only AMOLED visibility, background
voice work continues, touch does not wake it, and the display-off hold nests
with all existing SD pauses. Preserve keyboard and XiaoZhi behavior.

- [x] **Step 2: Run all Recorder host tests**

Run every command in `docs/recorder-design-guardrails.md`, plus the keyboard
PMIC decoder test. Expected: all commands exit 0.

- [x] **Step 3: Run the complete firmware build and hygiene checks**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
git diff --check
```

Expected: build exit 0, only `ns_create`, and no whitespace errors.

- [ ] **Step 4: Flash and verify on the real device**

Flash formal main, then use a temporary acceptance build or serial-assisted
physical presses to capture, in order:

```text
Agent voice WSS ready
PWR screen off
PWR screen on
```

Confirm the first press makes the AMOLED black, the second redraws the assistant
screen, WSS stays connected, no recording starts, and logs contain none of:
`spi_hal_setup_trans`, `Stack protection fault`, `Guru Meditation`, queue
overflow, or reboot loop. Serial must open with `dtr=True, rts=False`.

- [ ] **Step 5: Mark the plan complete and commit docs**

```bash
git add README.md AGENTS.md \
  main/boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md \
  docs/recorder-design-guardrails.md
git add -f docs/superpowers/plans/2026-07-12-recorder-pwr-screen-toggle.md
git commit -m "docs(recorder): document PWR screen toggle"
```
