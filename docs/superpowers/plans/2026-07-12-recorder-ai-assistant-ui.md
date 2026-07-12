# Recorder AI Assistant UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Recorder utility presentation with a stable single-action `金山 AI` voice-assistant interface while preserving the existing Azure, SD, recording, and playback behavior.

**Architecture:** Add a pure, host-tested presentation model that derives labels, colors, controls, and visibility from the existing Recorder/Agent states. Rebuild the fixed LVGL widget tree around that model, then route the one primary button back into the existing reducer events; storage, network, protocol, codec, and durable turn code remain unchanged.

**Tech Stack:** C++17, ESP-IDF 5.5.3, LVGL 9, ESP32-C6, host C++ tests, FATFS/SDSPI.

---

## File Structure

- Create `main/apps/recorder/recorder_assistant_ui.h`: presentation input/output types, semantic primary actions, notices, and color constants.
- Create `main/apps/recorder/recorder_assistant_ui.cc`: pure priority/mapping logic for Recorder mode, Agent phase, pending turns, errors, timer, and volume.
- Create `main/apps/recorder/recorder_assistant_ui_test.cc`: exhaustive host assertions for ready, offline, listening, busy, speaking, paused, queued, setup, storage, and retry views.
- Create `main/apps/recorder/font_puhui_assistant_24_4.c`: generated Puhui subset for the exact Chinese assistant copy and ASCII.
- Modify `main/apps/recorder/recorder_display.h`: replace split text/button state APIs with one render-model API.
- Modify `main/apps/recorder/recorder_display.cc`: build the `金山 AI` header, status pill, layered orb, single primary button, secondary history button, and restyled history overlay.
- Modify `main/apps/recorder/recorder_app.cc`: construct render inputs at existing state transitions and route the display model without changing I/O ordering.
- Modify `main/apps/recorder/recorder_file_list.h/.cc`: provide semantic history row labels.
- Modify `main/apps/recorder/recorder_playback_menu_test.cc`: prove `AI 回复`, `你`, and legacy labels.
- Modify `main/CMakeLists.txt`: compile the presentation module into firmware.
- Modify `docs/recorder-design-guardrails.md`, `README.md`, and `AGENTS.md`: document the assistant interaction and required regressions.

### Task 1: Add the Pure Assistant Presentation Model

**Files:**
- Create: `main/apps/recorder/recorder_assistant_ui.h`
- Create: `main/apps/recorder/recorder_assistant_ui.cc`
- Create: `main/apps/recorder/recorder_assistant_ui_test.cc`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the failing presentation-model test**

Create a host test that builds inputs with the existing authoritative enums and checks exact semantic output:

```cpp
#include "recorder_assistant_ui.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

RecorderAssistantUiInput Base() {
    RecorderAssistantUiInput input;
    input.mode = RecorderControlMode::kIdle;
    input.voice_phase = AgentVoicePhase::kOnline;
    input.sd_ready = true;
    input.noise_reduction_ready = true;
    input.volume = 70;
    return input;
}

void TestReady() {
    const auto model = RecorderBuildAssistantUi(Base());
    Check(model.connection_label == "已连接", "ready is online");
    Check(model.title == "准备好了", "ready title");
    Check(model.primary_label == "点击说话", "ready action");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kTalk,
          "ready starts talk");
    Check(model.primary_enabled && model.history_visible,
          "ready controls are available");
}

void TestListening() {
    auto input = Base();
    input.mode = RecorderControlMode::kRecording;
    input.elapsed_seconds = 65;
    const auto model = RecorderBuildAssistantUi(input);
    Check(model.title == "正在聆听", "listening title");
    Check(model.metric == "01:05", "listening timer");
    Check(model.primary_label == "发送", "listening sends");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kSend,
          "listening stop action");
    Check(!model.history_visible, "history hidden while listening");
}

void TestBusyAndQueued() {
    for (AgentVoicePhase phase : {AgentVoicePhase::kSending,
                                  AgentVoicePhase::kThinking,
                                  AgentVoicePhase::kReceiving}) {
        auto input = Base();
        input.voice_phase = phase;
        input.turn_pending = true;
        const auto model = RecorderBuildAssistantUi(input);
        Check(!model.primary_enabled, "busy action disabled");
        Check(model.primary_action == RecorderAssistantPrimaryAction::kNone,
              "busy has no primary action");
        Check(!model.history_visible, "busy history hidden");
    }
    auto queued = Base();
    queued.voice_phase = AgentVoicePhase::kOffline;
    queued.turn_pending = true;
    Check(RecorderBuildAssistantUi(queued).title == "已排队",
          "offline pending turn is queued");
}

void TestPlaybackAndFailures() {
    auto speaking = Base();
    speaking.mode = RecorderControlMode::kPlaying;
    speaking.volume = 80;
    auto model = RecorderBuildAssistantUi(speaking);
    Check(model.title == "正在播报" && model.metric == "音量 80",
          "playing view");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kPause,
          "playing pauses");

    speaking.mode = RecorderControlMode::kPaused;
    model = RecorderBuildAssistantUi(speaking);
    Check(model.title == "已暂停", "paused title");
    Check(model.primary_action == RecorderAssistantPrimaryAction::kResume,
          "paused resumes");

    auto no_sd = Base();
    no_sd.sd_ready = false;
    model = RecorderBuildAssistantUi(no_sd);
    Check(model.title == "请插入存储卡", "missing SD is explicit");
    Check(!model.primary_enabled, "missing SD blocks talk");

    auto setup = Base();
    setup.notice = RecorderAssistantNotice::kWifiSetup;
    Check(RecorderBuildAssistantUi(setup).title == "请设置网络",
          "Wi-Fi setup is explicit");
    setup.notice = RecorderAssistantNotice::kAgentSetup;
    Check(RecorderBuildAssistantUi(setup).title == "请设置助手",
          "Agent setup is explicit");
}
}  // namespace

int main() {
    TestReady();
    TestListening();
    TestBusyAndQueued();
    TestPlaybackAndFailures();
    std::puts("recorder_assistant_ui_test passed");
    return 0;
}
```

- [ ] **Step 2: Run the model test and verify RED**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_assistant_ui_test.cc \
  main/apps/recorder/recorder_assistant_ui.cc \
  -o /tmp/recorder_assistant_ui_test
```

Expected: compilation fails because the new presentation files and types do not exist.

- [ ] **Step 3: Implement the model types and priority rules**

Define these public types in `recorder_assistant_ui.h`:

```cpp
enum class RecorderAssistantPrimaryAction { kNone, kTalk, kSend, kPause, kResume };
enum class RecorderAssistantNotice { kNone, kWifiSetup, kAgentSetup, kDspFailure, kSaveFailure };

struct RecorderAssistantUiInput {
    RecorderControlMode mode = RecorderControlMode::kIdle;
    AgentVoicePhase voice_phase = AgentVoicePhase::kOffline;
    bool turn_pending = false;
    bool sd_ready = false;
    bool noise_reduction_ready = false;
    RecorderAssistantNotice notice = RecorderAssistantNotice::kNone;
    uint32_t elapsed_seconds = 0;
    int volume = 70;
};

struct RecorderAssistantUiModel {
    std::string connection_label;
    std::string title;
    std::string subtitle;
    std::string metric;
    std::string primary_label;
    uint32_t accent_rgb = 0x58D6FF;
    uint32_t connection_rgb = 0x58D6FF;
    RecorderAssistantPrimaryAction primary_action = RecorderAssistantPrimaryAction::kNone;
    bool primary_enabled = false;
    bool history_visible = false;
};

RecorderAssistantUiModel RecorderBuildAssistantUi(
    const RecorderAssistantUiInput& input);
```

Implement priority in this exact order: missing SD, explicit notice, recording,
playing/paused, pending/busy turn, then idle connectivity. Clamp volume to 0–100
and format elapsed time as zero-padded `MM:SS`. Use the palette from the design
spec and these exact activity titles: `准备好了`, `离线可用`, `正在聆听`,
`已排队`, `正在发送`, `正在思考`, `准备回复`, `正在播报`, `已暂停`,
`正在重试`, `请插入存储卡`, `请设置网络`, `请设置助手`, `录音不可用`,
and `保存失败`.

- [ ] **Step 4: Generate the bounded Chinese font subset**

Use Xiaozhi's existing `managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf`
with the 78/lv_font_conv tool to generate a 24 px, 4 bpp LVGL C font named
`font_puhui_assistant_24_4`. Include ASCII `0x20-0x7e` and every Chinese character
appearing in the presentation model, `金山 AI`, `对话历史`, `AI 回复`, and `你`.
Commit the generated C source; do not edit or commit `managed_components`.

- [ ] **Step 5: Add the sources to firmware and verify GREEN**

Append `apps/recorder/recorder_assistant_ui.cc` and
`apps/recorder/font_puhui_assistant_24_4.c` beside the other Recorder sources
in `main/CMakeLists.txt`, then run the command from Step 2 followed by:

```bash
/tmp/recorder_assistant_ui_test
```

Expected: `recorder_assistant_ui_test passed`.

- [ ] **Step 6: Commit the model and font subset**

```bash
git add main/CMakeLists.txt main/apps/recorder/recorder_assistant_ui.*
git commit -m "feat(recorder): model AI assistant presentation"
```

### Task 2: Rebuild the Fixed LVGL Interface Around One Primary Action

**Files:**
- Modify: `main/apps/recorder/recorder_display.h`
- Modify: `main/apps/recorder/recorder_display.cc`

- [ ] **Step 1: Add the unified render contract**

Include `recorder_assistant_ui.h` from `recorder_display.h` and replace
`RecorderShowText()` / `RecorderDisplaySetState()` with:

```cpp
void RecorderDisplayRenderAssistant(const RecorderAssistantUiModel& model);
```

Keep pause/resume and history overlay APIs unchanged. The display callback struct
continues exposing `record`, `stop`, `open_menu`, `pause_resume`, `exit`, and
`play_file`; the one button dispatches to these existing callbacks based on the
model's `primary_action`.

- [ ] **Step 2: Build the assistant widget tree**

Replace the old title/subtitle plus separate REC/PLAY/action buttons with fixed
widgets for:

```text
MENU           金山 AI             [● 已连接]

                 ( orb )
              准备好了
               请说话

               点击说话
                 历史
```

Use `font_puhui_assistant_24_4` for Chinese labels. Use screen background
`0x090D16`; create two non-clickable circular orb layers,
an `AI` center label, status/title/subtitle/metric labels, one 320×66 primary
button, and one 150×42 history button. Do not create LVGL animations or timers.
Add pressed-state styling to active buttons and disabled opacity to the primary
button. Keep all coordinates within the 480×480 target.

- [ ] **Step 3: Implement model rendering and primary dispatch**

Store the latest semantic primary action in the display module. On primary click:

```cpp
switch (s_primary_action) {
    case RecorderAssistantPrimaryAction::kTalk:
        if (s_callbacks.record) s_callbacks.record(s_callback_user_data);
        break;
    case RecorderAssistantPrimaryAction::kSend:
        if (s_callbacks.stop) s_callbacks.stop(s_callback_user_data);
        break;
    case RecorderAssistantPrimaryAction::kPause:
    case RecorderAssistantPrimaryAction::kResume:
        if (s_callbacks.pause_resume) s_callbacks.pause_resume(s_callback_user_data);
        break;
    case RecorderAssistantPrimaryAction::kNone:
        break;
}
```

`RecorderDisplayRenderAssistant()` updates all labels/colors/visibility in one
LVGL lock, applies or clears `LV_STATE_DISABLED`, and hides the history button when
the model says it is unavailable. It must not touch SD, network, or codec objects.

- [ ] **Step 4: Restyle the history overlay**

Rename its title to `对话历史`, retain `BACK`, and use the assistant
palette. Keep the existing scrollable list and path storage so callback pointers
remain stable.

- [ ] **Step 5: Run syntax/static checks and commit**

Run:

```bash
git diff --check
rg -n '金山 AI|点击说话|对话历史' \
  main/apps/recorder/recorder_display.cc
```

Expected: all three assistant strings exist and no whitespace errors are reported.

```bash
git add main/apps/recorder/recorder_display.h \
  main/apps/recorder/recorder_display.cc
git commit -m "feat(recorder): render single-action assistant UI"
```

### Task 3: Drive the New Interface From Existing Runtime State

**Files:**
- Modify: `main/apps/recorder/recorder_app.cc`

- [ ] **Step 1: Add one render helper and explicit transient notices**

Maintain `RecorderAssistantNotice ui_notice` plus a monotonic expiration for DSP
and save failures. Add a local renderer that constructs `RecorderAssistantUiInput`
from `control.mode`, `voice_state.phase`, `voice_state.queued_turn`, `sd_ok`, noise
reduction availability, elapsed recording seconds, and codec volume, then calls
`RecorderDisplayRenderAssistant(RecorderBuildAssistantUi(input))`.

- [ ] **Step 2: Replace utility text updates at every transition**

Render after initialization, Wi-Fi/socket state changes, WSS ready, turn upload,
turn accepted, reply start, reply store, playback start/pause/resume/volume change,
playback finish, recording start/timer/finish, and recoverable errors. Keep each
existing pause/resume pair around SD operations unchanged. Do not move file,
network, hash, or codec calls into display callbacks.

- [ ] **Step 3: Preserve interaction semantics through the primary button**

The primary button must publish the same existing reducer events:

- `kTalk` → `kTouchRecord`;
- `kSend` → `kTouchStop`;
- `kPause`/`kResume` → `kTouchPauseResume`.

Busy-state taps do not publish an event because the display is disabled; reducer
guards remain unchanged as defense in depth. History continues publishing
`kTouchPlay`.

- [ ] **Step 4: Run reducer/model regressions and commit**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_control_state_test.cc \
  main/apps/recorder/recorder_control_state.cc \
  main/apps/recorder/agent_voice_state.cc \
  -o /tmp/recorder_control_state_test && /tmp/recorder_control_state_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_assistant_ui_test.cc \
  main/apps/recorder/recorder_assistant_ui.cc \
  -o /tmp/recorder_assistant_ui_test && /tmp/recorder_assistant_ui_test
```

Expected: both tests pass.

```bash
git add main/apps/recorder/recorder_app.cc
git commit -m "feat(recorder): present Agent turns as conversation"
```

### Task 4: Make Stored Audio Read as Conversation History

**Files:**
- Modify: `main/apps/recorder/recorder_file_list.h`
- Modify: `main/apps/recorder/recorder_file_list.cc`
- Modify: `main/apps/recorder/recorder_playback_menu_test.cc`
- Modify: `main/apps/recorder/recorder_app.cc`

- [ ] **Step 1: Write failing semantic-label assertions**

Add to the playback menu test:

```cpp
Check(RecorderConversationLabel(entries[0]) == "AI 回复",
      "assistant audio is an AI reply");
Check(RecorderConversationLabel(entries[1]) == "你",
      "user audio is labeled as the user");
RecorderFileEntry legacy;
legacy.name = "rec7.wav";
Check(RecorderConversationLabel(legacy) == "录音",
      "legacy audio has a neutral label");
```

- [ ] **Step 2: Run the history test and verify RED**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test
```

Expected: compilation fails because `RecorderConversationLabel` is undefined.

- [ ] **Step 3: Implement and use semantic labels**

Add:

```cpp
std::string RecorderConversationLabel(const RecorderFileEntry& entry) {
    if (!entry.turn_id.empty() && entry.name == "assistant.wav") return "AI 回复";
    if (!entry.turn_id.empty() && entry.name == "user.wav") return "你";
    return "录音";
}
```

Use this helper in `ShowPlaybackMenu()` and keep the existing detail/path values.

- [ ] **Step 4: Verify GREEN and commit**

Build and run `/tmp/recorder_playback_menu_test`; expect
`recorder_playback_menu_test passed`.

```bash
git add main/apps/recorder/recorder_file_list.h \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_app.cc
git commit -m "feat(recorder): label audio as conversation history"
```

### Task 5: Documentation, Full Gate, Flash, and Real-Device Acceptance

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `docs/recorder-design-guardrails.md`
- Modify: `docs/superpowers/plans/2026-07-12-recorder-ai-assistant-ui.md`

- [ ] **Step 1: Update durable documentation**

Describe `金山 AI`, the single `点击说话`/`发送` control, state progression,
disabled busy interaction, assistant history labels, static rendering, and the
unchanged SPI2/SD safety boundary. Replace user instructions that describe separate
`REC`, `STOP`, and `PLAY` buttons.

- [ ] **Step 2: Run every Recorder host regression**

Run all commands in `docs/recorder-design-guardrails.md`, including the new
`recorder_assistant_ui_test`, control state, display area, rate converter, noise
reducer, playback menu, Agent state, turn store, FATFS replacement, protocol, and
network event tests.

Expected: every executable prints its pass marker and exits zero.

- [ ] **Step 3: Build the complete ESP32-C6 firmware**

Run without displaying `sdkconfig`:

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

Expected: firmware builds successfully for the configured Waveshare ESP32-C6
board. Do not print, copy, or commit `sdkconfig`; verify only that it remains
ignored and non-empty.

- [ ] **Step 4: Commit docs and completed plan**

Mark every completed step in this plan, then run:

```bash
git diff --check
git status --short --branch
git add README.md AGENTS.md docs/recorder-design-guardrails.md
git add -f docs/superpowers/plans/2026-07-12-recorder-ai-assistant-ui.md
git commit -m "docs(recorder): document AI assistant interaction"
```

- [ ] **Step 5: Flash and inspect the real device**

Run:

```bash
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem1101 flash
```

Use safe serial listening (`dtr=True`, `rts=False`) and visual inspection. Confirm
the `金山 AI` identity, single-button states, history labels, and absence of SPI,
stack, reset, hash, or queue failures.

- [ ] **Step 6: Complete one physical voice turn**

On the device, tap `点击说话`, speak a weather question, tap `发送`, and observe
the state sequence through `Speaking`. Run:

```bash
/tmp/ser-venv/bin/python scripts/verify_agent_voice_runtime.py \
  --port-glob /dev/cu.usbmodem1101 --duration 30
```

Expected: `PASS: Agent voice reply was stored and playback started` and no fatal
marker. This final physical tap remains an external acceptance action; all software,
build, flash, and safe-listening work proceeds autonomously.
