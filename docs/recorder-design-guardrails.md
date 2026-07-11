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
- Recorder mode keeps SD logging disabled and only logs to serial.

## 2. ESP-SR Noise Suppression on ESP32-C6

ESP-SR `ns_pro_create()` was not safe on the ESP32-C6 target in this firmware.

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
- On ESP32-C6, keep the recorder on the fallback software enhancer unless a
  new true-hardware test proves ESP-SR NS is safe.
- Any future DSP library integration must have a real-device boot test before
  being enabled by default.

Current guard implementation:

- `RecorderNoiseReducer` disables ESP-SR NS on `CONFIG_IDF_TARGET_ESP32C6`.
- The fallback path uses noise gate, gain, and limiter so recording remains
  usable without crashing.

## 3. Verification Required for Recorder Changes

Before calling recorder work complete, run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_noise_reducer_test.cc \
  main/apps/recorder/recorder_noise_reducer.cc \
  -o /tmp/recorder_noise_reducer_test && /tmp/recorder_noise_reducer_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/recorder \
  main/apps/recorder/recorder_playback_menu_test.cc \
  main/apps/recorder/recorder_file_list.cc \
  main/apps/recorder/recorder_wav_file.cc \
  -o /tmp/recorder_playback_menu_test && /tmp/recorder_playback_menu_test

source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor
```

Real-device acceptance checks:

- Recorder boot reaches the idle screen without reboot loops for at least
  15 seconds.
- Logs do not contain `spi_hal_setup_trans`, `Guru Meditation`, or `Rebooting`.
- SD card mounts and self-test passes.
- Recording start/stop saves a WAV.
- `PLAY` opens the recordings menu.
- Selecting a WAV plays audio without rebooting.
