# Recorder Design Guardrails

This document records hardware and library pitfalls found during the 2026-07-11
recorder playback/noise-reduction work. Check this before designing or adding
new recorder features.

## 1. LCD and SD Share SPI2

The Waveshare ESP32-C6 Touch AMOLED 2.16 uses the same SPI2 bus for the AMOLED
display and the SD card.

Symptoms seen on hardware:

- Screen appeared to flash continuously.
- Serial logs showed repeated reboot loops.
- Panic was:
  `assert failed: spi_hal_setup_trans ... (spi_ll_get_running_cmd(hw) == 0)`.
- Backtrace entered `sdspi_host_*` while LVGL display flush could also access
  the LCD on SPI2.

Design rule:

- Never run SD card transfers while LVGL can flush the display on this board.
- For recorder mode, pause LVGL before SD-heavy work and resume it after:
  SD mount/self-test, recording writes, WAV playback reads, and serial WAV dump.
- Do not enable SD-card log mirroring in recorder mode. The log hook can write
  to SD from arbitrary `ESP_LOG*` calls and race with display flush.
- Prefer staging screen text/object updates while LVGL is paused, then resume
  once so the display refresh happens outside SD I/O.

Current guard implementation:

- `RecorderDisplayPause()` / `RecorderDisplayResume()` in
  `main/apps/recorder/recorder_display.*`.
- Recorder mode enables the AXP2101 IRQ2 short-press bit. Its PMIC polling task
  only publishes `kPhysicalPower`; the Recorder main task performs SH8601
  display-off/on. Screen-off holds one persistent LVGL pause that nests with
  every SD pause, while recording, playback, Wi-Fi, WSS, Agent work and SD
  persistence continue. Touch does not wake the stopped LVGL task.
- Recorder-mode SH8601 invalidation areas are expanded to even start
  coordinates and odd end coordinates. Without this panel requirement, the
  old PAUSE border can remain as two colored scanlines on the PLAY DONE screen.
- Recording keeps LVGL running while waiting for microphone PCM and processing
  DSP, then pauses it only around each bounded WAV write.
- Playback reads at most 1024 source samples with LVGL paused, resumes before
  conversion/I2S output, and performs no SD or I2S work while paused by the
  user.
- Recorder mode keeps SD logging disabled and only logs to serial.

## 2. ESP-SR Noise Suppression on ESP32-C6

ESP-SR's full AFE path is not selected for ESP32-C6 in XiaoZhi, and
`ns_pro_create()` was not safe on the ESP32-C6 target in this firmware.

Symptoms seen on hardware:

- Recorder boot reached `codec ready`.
- Panic followed immediately:
  `Guru Meditation Error: Core 0 panic'ed (Store access fault)`.
- Backtrace:
  `WebRtcNs_set_policy_core -> ns_pro_create -> RecorderNoiseReducer`.
- The NS handle was effectively null inside the ESP-SR library before policy
  setup.

Design rule:

- Do not assume ESP-SR NS/AGC is available just because the component builds.
- Do not use the full AFE, `ns_pro_create()`, or ESP-SR AGC in the ESP32-C6
  recorder. The `ns_pro_create()` WebRTC path needs a large contiguous
  internal-RAM allocation and did not safely handle allocation failure here.
- The lower-memory official `ns_create(10)` path is allowed only when a
  real-device cold-boot log proves that it initialized and stayed stable, and
  a fixed acoustic recording proves that speech is preserved.
- If `ns_create(10)` returns null, keep streaming rate conversion and fixed
  gain/limiting active, but surface `NS OFF`; do not substitute a sample
  threshold gate and call it noise suppression.

Current guard implementation:

- `RecorderNoiseReducer` converts the 24 kHz microphone stream to 16 kHz,
  sends exact 160-sample frames through `ns_create(10)` / `ns_process()`, then
  applies fixed gain and a peak limiter.
- New recordings are 16 kHz mono PCM16. Playback accepts both those files and
  legacy 24 kHz mono PCM16 files, resampling only when needed.
- Boot logs include internal-heap measurements and the actual NS state. Source
  and linked-symbol checks must show `ns_create`, never `ns_pro_create`.

## 3. Verification Required for Recorder Changes

Agent voice additions keep the same SPI2 ownership rule:

- WebSocket/Wi-Fi callbacks only copy bounded events; they never access FATFS, LVGL or the codec.
- The recorder main task pauses LVGL around each SD read/write. New firmware uses one authenticated same-origin HTTPS GET for the entire reply; bounded reads are local buffering, not extra download requests. Only the legacy WSS fallback sends a per-chunk ACK.
- `assistant.wav.part` is never listed or played. Startup pending scans remove stale parts and request an idempotent replay.
- Recorder input reserves DSP flush capacity below the 4 MiB protocol maximum and auto-stops at the limit.
- Processor errors are not all equivalent. A control frame with `retryable=true` keeps the existing reconnect/replay path. A frame with `retryable=false` is terminal for the active turn: abort any partial reply, atomically persist `failed` plus the stable error code, clear the queued reducer state, and continue with the next pending turn or online idle. `ListPending()` must exclude `failed`, while its `user.wav` remains on SD for history and diagnosis.
- The protocol-v1 authenticated ready frame may carry `server_time_ms` and `timezone_offset_minutes`. `RecorderTurnClock` combines that baseline with elapsed monotonic time for `YYYYMMDD`, created time and fixed-width hexadecimal IDs. Legacy ready frames remain valid; an offline first recording uses `unsynced`. Do not use `%llu` in IDs or represent an unavailable wall clock as `19700101`.

Runtime concurrency (2026-10-13):

- `RecorderTurnQueue` holds the immutable in-flight turn at its front. Only startup calls the recovering `ListPending()` scan; appending a new recording must not unlink a receiving `.part`.
- Process at most one network event and one upload chunk per loop. Reuse a heap buffer for uploads: a 4 KiB local array exhausts the 8 KiB main stack during TLS setup.
- `RecorderPlaybackStream` reads bounded blocks and retains its cursor across microphone interruptions. Transport completion advances the send queue independently of playback completion.
- Input opens only for recording; output is disabled while recording. FATFS remains on the main task with nested display pause protection.

The user-facing recorder presentation is a voice assistant, not a recording utility:

- The selector entry is `Shenyong AI`; the main screen identity is `神勇`. New audio and history use `/sdcard/shenyong/`; old Jinshan directories are not replayed or displayed.
- One primary button uses `PRESSED` to start recording and `RELEASED` / `PRESS_LOST` to send. Its labels are `按住说话` / `松开发送`; sending, thinking, receiving and playback do not block new recordings. Latch the press independently of presentation changes so release always stops the microphone.
- Empty STT uses the terminal notice `没有听清` / `请再说一次`; the talk action is immediately available instead of replaying the same WAV forever. A prior-turn notice must never hide the release action during a new recording.
- `RecorderBuildAssistantUi()` is the only presentation-state mapper. Display callbacks publish existing reducer events and never access SD, network, or codec objects.
- The UI uses a fixed LVGL object tree without animations or UI timers. Runtime changes only update text, colors, visibility, and disabled state.
- Chinese fixed copy uses `font_puhui_assistant_24_4`, generated as a bounded subset from XiaoZhi's `puhui-common.ttf`. Regenerate the subset whenever fixed Chinese copy changes.
- The current simplified UI has no history control or file browser and does not load the dynamic assets font. Fixed copy uses the 24 px subset, and the brand/action use a 36 px subset. Retained history utility tests only protect older persisted formats; they do not imply a visible feature.
- On display changes, an active touch is handed between LVGL and dark polling without starting or stopping a second turn. Release remains authoritative.


ESP-IDF FATFS does not provide POSIX replacement semantics for `rename()`:

- Renaming `.part` directly onto an existing `turn.json` or `assistant.wav`
  returns `FR_EXIST`. Host filesystems normally replace the destination, so an
  ordinary macOS test does not reproduce this failure.
- Durable publication writes and syncs `.part`, moves an existing primary to
  `.bak`, publishes `.part`, restores the backup on failure, then removes the
  backup after success.
- If power is lost after the primary moved to `.bak`, startup reads the backup
  manifest and the next state update republishes the primary. `.part` and
  `.bak` are never listed or played.

Before calling recorder work complete, run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_turn_clock_test.cc \
  main/apps/recorder/recorder_turn_clock.cc \
  -o /tmp/recorder_turn_clock_test && /tmp/recorder_turn_clock_test

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

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_display_area_test.cc \
  main/apps/recorder/recorder_display_area.cc \
  -o /tmp/recorder_display_area_test && /tmp/recorder_display_area_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_history_layout_test.cc \
  main/apps/recorder/recorder_history_layout.cc \
  -o /tmp/recorder_history_layout_test && /tmp/recorder_history_layout_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_rate_converter_test.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/recorder_rate_converter_test && /tmp/recorder_rate_converter_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_noise_reducer_test.cc \
  main/apps/recorder/recorder_noise_reducer.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/recorder_noise_reducer_test && /tmp/recorder_noise_reducer_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/agent_turn_manifest.cc \
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test && /tmp/recorder_playback_menu_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_common_font_test.cc \
  main/apps/recorder/recorder_common_font.cc \
  -o /tmp/recorder_common_font_test && /tmp/recorder_common_font_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/agent_voice_state_test.cc \
  main/apps/recorder/agent_voice_state.cc \
  -o /tmp/agent_voice_state_test && /tmp/agent_voice_state_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/agent_turn_store_test.cc \
  main/apps/recorder/agent_turn_store.cc \
  -o /tmp/agent_turn_store_test && /tmp/agent_turn_store_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -Drename=AgentTurnStoreFatRename \
  -I main/apps/recorder \
  main/apps/recorder/agent_turn_store_fatfs_test.cc \
  main/apps/recorder/agent_turn_store.cc \
  -o /tmp/agent_turn_store_fatfs_test && /tmp/agent_turn_store_fatfs_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/agent_voice_protocol_test.cc \
  main/apps/recorder/agent_voice_protocol.cc \
  -o /tmp/agent_voice_protocol_test && /tmp/agent_voice_protocol_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_network_events_test.cc \
  main/apps/recorder/recorder_network.cc \
  -DRECORDER_NETWORK_HOST_TEST \
  -o /tmp/recorder_network_events_test && /tmp/recorder_network_events_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_concurrency_test.cc \
  main/apps/recorder/recorder_playback_stream.cc \
  main/apps/recorder/recorder_wav_file.cc \
  main/apps/recorder/agent_turn_store.cc \
  -o /tmp/recorder_concurrency_test && /tmp/recorder_concurrency_test

c++ -std=c++17 -Wall -Wextra -Werror \
  main/apps/recorder/recorder_hold_input_test.cc \
  -o /tmp/recorder_hold_input_test && /tmp/recorder_hold_input_test

c++ -std=c++17 -Wall -Wextra -Werror -DRECORDER_REPLY_DOWNLOAD_HOST_TEST \
  main/apps/recorder/recorder_reply_download_test.cc \
  main/apps/recorder/recorder_reply_download.cc \
  -o /tmp/recorder_reply_download_test && /tmp/recorder_reply_download_test

c++ -std=c++17 -Wall -Wextra -Werror \
  main/apps/recorder/recorder_amr_encoder_test.cc \
  main/apps/recorder/recorder_amr_encoder.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/recorder_amr_encoder_test && /tmp/recorder_amr_encoder_test

c++ -std=c++17 -Wall -Wextra -Werror \
  main/apps/recorder/recorder_display_guard_test.cc \
  -o /tmp/recorder_display_guard_test && /tmp/recorder_display_guard_test

source ~/esp/esp-idf/export.sh
idf.py build
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
idf.py -p /dev/cu.usbmodem1101 flash
/tmp/ser-venv/bin/python scripts/verify_agent_voice_runtime.py \
  --port-glob /dev/cu.usbmodem1101 --duration 30
```

Real-device acceptance checks:

- Shenyong AI boot reaches `神勇` / `准备好了` without reboot loops for at least 15 seconds.
- Logs show `ESP-SR NS enabled` and
  `recorder DSP: input=24000 output=16000 ns=enabled`.
- Logs do not contain `spi_hal_setup_trans`, `Guru Meditation`, or `Rebooting`.
- SD card mounts and self-test passes.
- Holding `按住说话` enters `正在聆听`; releasing saves a 16 kHz mono PCM16 WAV whose environment-only regions are audibly quieter without clipped or missing speech. BOOT/KEY adjust volume without ending recording.
- During a connected turn, the screen progresses through `正在发送`, `正在思考`, `准备回复`, and `正在播报`; the primary button remains available while busy. Record a second turn before the first reply and verify both retain the correct IDs and receive replies in order.
- The page has only status and a single large push-to-talk target; no history, decorative orb or separate pause/resume controls.

- Both a legacy 24 kHz WAV and a new 16 kHz WAV play without rebooting.
- Holding during playback stops audible output and records; releasing resumes from the same position. Replies arriving during recording wait until release. The screen, heartbeat and chunk ACKs remain responsive throughout.
- Playback left/right clicks adjust volume by +10/-10 and clamp it to 0–100.
- A short `MENU` press does nothing; a continuous two-second hold returns to
  the application selector.
- A PWR short press turns only the AMOLED off, a second short press turns it on
  and redraws the current assistant state. Background voice and network work
  continue while dark, and touch does not wake the screen.

## 2026-10-13 dark-screen operation

As of 2.2.11, KEY/GPIO10 increases volume by ten and BOOT/GPIO9 decreases it by ten
on each press-down, clamped to 0–100. Both work in every mode and display state;
neither key records or changes the panel/network state. Touch remains the recording input.

While dark, LVGL remains stopped. `RecorderDisplayPollDarkTouch()` is called from
the same main loop and reads CST9217 through I2C at most every 20 ms. A fresh
300 ms hold anywhere starts recording; release sends without waking the panel.
Screen-transition stale touches and brief taps are ignored. The screen-off pause
continues to nest with SD read/write pauses.

Offline end-to-end storage must accept the exact `unsynced` date produced by
`RecorderTurnClock`; `agent_turn_store_test` checks creation and restart recovery.

## Whole-file reply download (2.2.7)

The user explicitly replaced stop-and-wait reply downloads with a single HTTPS GET.
`RecorderReplyDownload` uses `esp_http_client`, the platform certificate bundle and the
same device credential. It rejects redirects, non-200 status, incorrect Content-Length
and non-matching turn paths. Reads are bounded, HTTPS handshake/receive runs on a separate 12 KiB task with an 8 KiB stream buffer; main-task reads never block, and
15 seconds without progress triggers an idempotent retry of the already-created file.
The complete byte count and SHA-256 must pass before atomic publication and playback.
The old WSS path remains only for server compatibility and is covered by existing tests.
8 kHz PCM16 mono playback is admitted and resampled to 24 kHz without changing capture
or the default server TTS format.

## AMR-NB playback and storage (2.2.8)

Negotiate Reply-Format amr-nb only with the full-file HTTPS transport. Reply metadata
must match AMR bytes/hash, sample rate 8000, and the exact format query. Publish as
assistant.amr and record that filename; never disguise AMR as WAV. Existing WAV files
and format checks remain supported. FATFS replacement tests cover both extensions.

Parse the six-byte AMR file magic and each frame boundary before decode; reject
reserved frame types, invalid padding and truncation. Use the target SDK's AMR-NB
frame decoder, retain it across recording interruptions, and resample its 160-sample
8 kHz output to the existing 24 kHz codec. A synthetic silence-frame boot self-test
checks the actual target decoder without emitting audio.

## Dark recording and AMR uplink (2.2.9)

The panel's persistent dark pause is a floor: SD operations may acquire/release their
own nested pause, but only explicit screen-on can release the last pause. Recording
preparation must never call Resume before acquiring its own pause. This fixes accidental
LVGL revival while the dark-touch main-task reader is active.

Encode denoised 16 kHz PCM incrementally to AMR-NB at 8 kHz / 12.2 kbps. Store user.amr and
its exact compressed checksum; keep logical PCM counts for duration/UI. Legacy WAV manifests
remain valid and explicitly select their upload format. Run encoder framing/reset/failure,
manifest recovery, protocol, and dark-pause guard tests plus the target codec self-tests.

## AMR task stack (2.2.10)

On the real C6, AMR-NB MR122 codebook search (`code_10i40_35bits`, called by `cbsearch`)
overflows the 8 KiB ESP-IDF main task even for the startup silence frame. This causes a
reboot loop and visible flashing. `StartRecorderApp()` runs the entire recorder on one
24 KiB task and lets app_main return. Keep ownership of SD, codec, display and dark-touch
polling on that task; moving encoding into a concurrent SD/UI owner is not a fix.
Keep the target encoder self-test and log task minimum-free bytes after it and recordings.

## Sub-second gesture rejection (2.2.11)

Count captured input PCM frames, not wall-clock hold time or padded AMR frames.
Below one second, close and remove only the new unindexed recording, before flush/hash/
manifest publication/queue insertion. Both release and application exit use the same gate.
A cleanup failure still must not create a manifest, so restart cannot upload the rejected clip.
Do not scan or mutate the existing queue; an interrupted assistant reply resumes next loop.
