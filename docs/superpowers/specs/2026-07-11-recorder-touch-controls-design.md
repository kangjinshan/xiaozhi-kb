# Recorder Touch Controls Design

**Date:** 2026-07-11

**Goal:** Operate recorder mode from the touchscreen, provide pause/resume and
volume controls during playback, and move mode exit to an explicit two-second
press on a visible `MENU` button.

## Scope

This change affects only recorder-mode controls and playback scheduling. It
does not change the ESP-SR noise-suppression chain, WAV format, file naming, or
recordings menu ordering.

## User Interface

The 480 x 480 recorder screen has a visible `MENU` button in the upper-left
corner. A short press does nothing. Holding it continuously for two seconds
requests exit to the application selector. If recording is active, the
existing recording-finalization path flushes and closes the WAV before reboot.

The remaining controls depend on recorder state:

- **Idle:** show `REC` and `PLAY`. Tapping `REC` starts a new recording. Tapping
  `PLAY` opens the recordings list.
- **Recording:** replace the idle controls with one centered `STOP` button.
  Tapping it stops and saves the recording. Firmware-handled physical keys do
  nothing while recording.
- **Playing:** show one centered `PAUSE` button. Tapping it pauses at the
  current playback position and changes the label to `RESUME`; tapping again
  continues from that position. The subtitle shows the current volume.
- **Paused:** keep `RESUME` and `MENU` responsive without reading the SD card
  or sending more PCM to the codec.
- **Playback completed or failed:** return to the idle controls and show the
  existing completion or failure message.

The recordings overlay keeps its `BACK` control and presents the visible
`MENU` button in its header without overlapping the title.

## Physical-Key Mapping

Physical keys are state-dependent:

- Idle: no recording action is assigned to a physical key.
- Recording: all firmware-handled physical-key clicks are ignored.
- Playing or paused: left-key click requests volume `+10`; right-key click
  requests volume `-10`. Volume is clamped to 0–100 and shown on screen.
- The former right-key long-press exit behavior is removed. Recorder exit is
  available only through the two-second touchscreen `MENU` hold.

The board's middle PMIC power key is hardware-managed and is outside this
mapping.

## Components and State

`recorder_display` owns LVGL widgets and translates touch gestures into small
callbacks. It exposes a recorder display state (`idle`, `recording`,
`playing`, or `paused`) so widget visibility and labels are updated in one
place. Its `MENU` handler measures press duration with LVGL ticks and emits one
exit callback after 2000 ms; releasing early cancels the request.

`recorder_app` owns operational state and all codec, SD-card, and WAV work.
Touch and physical-key callbacks only publish requests. The recorder task
consumes those requests, changes state, performs file operations, adjusts
codec volume, and updates the display. Callbacks never read or write a WAV
directly.

A small platform-neutral control-state unit defines which requests are valid
in each state. This makes physical-key disabling, volume mapping, and
pause/resume transitions host-testable without LVGL or ESP-IDF.

## SPI2-Safe Interactive Playback

The AMOLED and SD card share SPI2, so LVGL must not flush while an SD transfer
is active. Playback therefore alternates ownership in this order:

1. Pause LVGL.
2. Read one bounded WAV block from SD.
3. Resume LVGL.
4. Resample the block if required and send it to I2S.
5. Consume pause, volume, recording, and exit requests.
6. Repeat from step 1 only when playback remains active.

Each playback read contains at most 4096 source samples. That bounds pause and
volume response to about 256 ms for 16 kHz input and 171 ms for 24 kHz input
while avoiding excessive LVGL pause/resume churn. While paused, LVGL remains
running and no SD or I2S work occurs. Before closing the file, playback pauses
LVGL again so the caller can update the final screen safely and then resume it
once.

The streaming sample-rate converter retains the largest output capacity seen
for a stream. This matches Espressif's fixed maximum-buffer example and keeps
a short final input block from undersizing the output buffer when the library
emits cached samples.

## SPI2-Safe Touch Recording

The touchscreen must remain active so `STOP` and `MENU` can be detected while
recording, but WAV writes still cannot overlap an LVGL flush. Each microphone
block therefore follows this order:

1. Keep LVGL running while waiting for microphone PCM and processing DSP.
2. Pause LVGL before appending processed PCM to the WAV.
3. Update elapsed-time text while LVGL is paused.
4. Resume LVGL immediately after the bounded SD write.
5. Consume touch and physical-key requests before reading the next block.

At the 1024-sample microphone block size, touch requests are serviced at least
once per roughly 43 ms of captured 24 kHz audio, plus DSP and bounded write
time. Starting, stopping, finalizing, and serial-dumping a recording keep LVGL
paused for the complete SD operation and resume it only after the operation is
safe.

## Error and Interruption Handling

- A touch `STOP` uses the same reducer flush, WAV-header rewrite, and save
  checks as any other recording stop.
- A recording write or DSP failure moves to the existing `SAVE FAILED` path.
- `MENU` during recording finalizes the WAV before exiting.
- `MENU` during playback interrupts playback, closes the file with LVGL
  paused, and exits.
- Pause does not flush or reset the rate converter; resume continues the same
  stream.

## Verification

Host tests must prove:

- idle touch `REC` starts recording;
- physical keys are ignored while recording;
- playback left/right clicks map to volume up/down and clamp at 0/100;
- touch pause and resume preserve the playback state;
- a short MENU press does not exit and a two-second hold does;
- existing rate-converter, noise-reducer, and WAV compatibility tests pass.

ESP32-C6 verification must prove:

- the firmware builds and links `ns_create`, not `ns_pro_create`;
- tapping `REC` and `STOP` creates a valid 16 kHz mono PCM16 WAV;
- recording ignores physical-key clicks;
- new 16 kHz and legacy 24 kHz WAV files both play to `PLAY DONE`;
- `PAUSE` stops audible playback, `RESUME` continues from the same position,
  and the screen remains responsive;
- playback volume keys work and remain within 0–100;
- a short `MENU` press is harmless and a two-second hold returns to the
  selector;
- logs contain no heap assertion, `Guru Meditation`, `Rebooting`, or
  `spi_hal_setup_trans` failure during the complete flow.
