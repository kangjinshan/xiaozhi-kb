# Recorder AI Assistant UI Design

**Date:** 2026-07-12
**Target:** Waveshare ESP32-C6-Touch-AMOLED-2.16 Recorder/Agent mode

## Goal

Turn the device-facing Recorder screen into a focused AI voice assistant while
preserving the proven Recorder → Azure Agent → SD → automatic playback pipeline.
The user should understand one primary action—start or finish speaking—and should
see the assistant's current state without reading transport-oriented terms.

## Product Direction

Three directions were considered:

1. Restyle the existing `REC`, `PLAY`, and `STOP` layout. This is low risk, but it
   still presents the device as a recording utility.
2. Use one context-sensitive conversation button, an assistant status center, and
   a secondary history entry. This changes the mental model while retaining the
   reliable tap-to-start/tap-to-send interaction.
3. Use press-and-hold-to-talk, continuous animation, and on-screen transcript
   cards. This feels more conversational, but raises accidental short-recording,
   font coverage, RAM, touch-loss, and shared-SPI display traffic risks.

Direction 2 is selected. It provides the clearest assistant identity without
weakening the already verified storage and transfer behavior.

## Experience Principles

- The screen says `JINSHAN AI`, not Recorder.
- One large primary control owns the current conversation action.
- Human state words replace protocol words: `Ready`, `Listening`, `Thinking`,
  `Preparing reply`, and `Speaking`.
- Network and storage are supporting status, not the center of the screen.
- Offline speech remains allowed and is described as queued for later delivery.
- Busy states visibly disable new speech instead of silently ignoring taps.
- History remains available but secondary to speaking with the assistant.
- No continuous animation is required. Static color/state changes and the existing
  one-second recording timer limit SPI2 display traffic.

## Main Screen

The 480×480 layout uses five stable regions:

1. Top left: small `MENU` control. A two-second hold still returns to the app
   selector; a short tap remains harmless.
2. Top center: `JINSHAN AI` identity label.
3. Top right: compact connectivity pill (`ONLINE`, `OFFLINE`, `CONNECTING`, or
   `RETRYING`) with a matching color dot.
4. Center: a layered circular assistant orb, a large activity title, and one line
   of guidance. The orb color changes by state but does not animate continuously.
5. Bottom: one wide primary button and a smaller `HISTORY` button.

Visual palette:

- background: near-black navy `#090D16`;
- assistant idle: cyan `#58D6FF`;
- listening: warm coral `#FF667A`;
- thinking/receiving: violet `#9D7CFF`;
- speaking: mint `#55E6A5`;
- offline/error: amber `#F5B94C`;
- primary text: `#F7FAFF`; secondary text: `#96A3B7`.

The existing Puhui fonts remain in use. User-facing strings are concise English so
the screen does not depend on adding a larger Chinese glyph asset in this change.

## Interaction Model

### Ready or Offline

- Primary button: `TAP TO TALK`.
- First tap starts recording.
- Offline mode still starts recording and explains `Saved locally - sends later`.
- `HISTORY` opens stored user/assistant audio only when no turn is pending.

### Listening

- Primary button becomes `SEND`.
- Center title shows `Listening` and the elapsed `MM:SS` timer.
- Second tap finishes and durably stores the user WAV, then queues or sends it.
- Physical volume keys remain inactive, matching the existing recorder guardrail.

### Sending, Thinking, and Receiving

- Primary button remains visible but disabled.
- The center title progresses through `Sending`, `Thinking`, and
  `Preparing reply`.
- Guidance explains that the question is safely stored while processing.
- A second recording cannot begin until the current turn finishes.

### Speaking and Paused

- Automatic reply playback shows `Speaking`.
- Primary button becomes `PAUSE`; after tapping it becomes `RESUME`.
- The volume value is shown as supporting status. Existing physical left/right
  volume controls and 0–100 clamping remain unchanged.

### Completion and Recovery

- After playback, the screen returns to `Ready` and `TAP TO TALK`.
- A queued offline turn shows `Queued` with a disabled primary control until the
  turn is delivered and played.
- Missing SD, missing Wi-Fi provisioning, missing Agent provisioning, DSP failure,
  save failure, and transport retry each receive a specific assistant-style error
  view. None of these views claims that audio was saved unless the durable store
  operation succeeded.

## Presentation Model and Boundaries

Add a small pure presentation module alongside the recorder state machines. It
maps an assistant UI phase plus elapsed time/volume into a render model containing:

- connectivity label and color;
- activity title and guidance;
- orb/accent colors;
- primary button label, enabled state, and semantic action;
- history visibility;
- optional recording timer or playback volume.

The presentation module contains no LVGL, SD, networking, codec, or FreeRTOS code
and is host-testable. `recorder_app.cc` remains the owner of real state transitions
and passes render models to `recorder_display.cc`. The display module owns widgets
and styling only; touch callbacks continue to publish requests and never perform
SD, network, WAV, or codec work.

The existing Recorder and Agent reducers remain authoritative. The redesign does
not add a second business state machine and does not change WSS protocol, turn
idempotency, ACK flow control, hashing, FATFS publication, or playback sequencing.

## History Screen

- Rename `Recordings` to `Conversation History` and `PLAY` to `HISTORY`.
- Label paired entries `AI REPLY` and `YOU` instead of exposing turn IDs as the
  primary text.
- Keep the existing detail text and playback behavior.
- Preserve legacy WAV compatibility.
- Keep `BACK` and the two-second `MENU` exit behavior.

## Failure Safety

- LVGL remains paused around every bounded SD access.
- Rendering occurs after state/data changes and before or after SD work using the
  existing pause-depth discipline.
- No animation timer performs display work while the recorder task owns SD.
- Disabled primary controls use both visual opacity and LVGL disabled state;
  reducer guards remain the final authority.
- `.part` and `.bak` files never appear in history or playback.
- If display initialization fails, the audio/storage pipeline continues with serial
  diagnostics exactly as it does today.

## Test and Acceptance Strategy

Host tests must prove every UI phase maps to the intended labels, colors, enabled
actions, history visibility, timer, and volume text. Existing recorder control,
Agent voice, file list, FATFS replacement, protocol, and network tests remain in
the required gate.

The full ESP-IDF build must pass for ESP32-C6. Real-device acceptance must verify:

- boot shows `JINSHAN AI` without a reboot loop;
- ready/offline screen has one obvious `TAP TO TALK` action;
- tap → listening timer → `SEND` stores the user WAV;
- sending/thinking/receiving states visibly disable new speech;
- reply storage transitions to `Speaking` and automatic playback;
- pause/resume and physical volume behavior still work;
- history presents `YOU`/`AI REPLY` entries and plays both;
- no SPI2 assertion, stack fault, Guru Meditation, hash failure, or event overflow;
- the existing six serial Agent runtime milestones still pass.

## Non-Goals

- Wake-word or always-listening capture.
- Barge-in while the assistant is processing or speaking.
- Streaming transcript or response text.
- Continuous waveform/orb animation.
- Changing Azure, Agent, MCP, Opus/GPT routing, WSS protocol, or SD schema.
- Adding new font assets or exposing device credentials in UI/logs.
