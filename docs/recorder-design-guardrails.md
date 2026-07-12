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
- Recorder-mode SH8601 invalidation areas are expanded to even start
  coordinates and odd end coordinates. Without this panel requirement, the
  old PAUSE border can remain as two colored scanlines on the PLAY DONE screen.
- Recording keeps LVGL running while waiting for microphone PCM and processing
  DSP, then pauses it only around each bounded WAV write.
- Playback reads at most 4096 source samples with LVGL paused, resumes before
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
- The recorder main task pauses LVGL around each SD read/write, then acknowledges a reply chunk only after its bytes are on the card.
- `assistant.wav.part` is never listed or played. Startup pending scans remove stale parts and request an idempotent replay.
- Recorder input reserves DSP flush capacity below the 4 MiB protocol maximum and auto-stops at the limit.

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
  main/apps/recorder/recorder_control_state_test.cc \
  main/apps/recorder/recorder_control_state.cc \
  -o /tmp/recorder_control_state_test && /tmp/recorder_control_state_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_display_area_test.cc \
  main/apps/recorder/recorder_display_area.cc \
  -o /tmp/recorder_display_area_test && /tmp/recorder_display_area_test

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
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test && /tmp/recorder_playback_menu_test

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

source ~/esp/esp-idf/export.sh
idf.py build
nm build/esp-idf/main/libmain.a | rg ' U ns_(create|pro_create)$'
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor
```

Real-device acceptance checks:

- Recorder boot reaches the idle screen without reboot loops for at least
  15 seconds.
- Logs show `ESP-SR NS enabled` and
  `recorder DSP: input=24000 output=16000 ns=enabled`.
- Logs do not contain `spi_hal_setup_trans`, `Guru Meditation`, or `Rebooting`.
- SD card mounts and self-test passes.
- Recording start/stop saves a 16 kHz mono PCM16 WAV whose environment-only
  regions are audibly quieter without clipped or missing speech.
- `REC` and `STOP` work by touch, and physical keys do nothing while recording.
- `PLAY` opens the recordings menu.
- Both a legacy 24 kHz WAV and a new 16 kHz WAV play without rebooting.
- `PAUSE` stops audible output, `RESUME` continues from the same position, and
  the screen stays responsive throughout.
- Playback left/right clicks adjust volume by +10/-10 and clamp it to 0–100.
- A short `MENU` press does nothing; a continuous two-second hold returns to
  the application selector.
