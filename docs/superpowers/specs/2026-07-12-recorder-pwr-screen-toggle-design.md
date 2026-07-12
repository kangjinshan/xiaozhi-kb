# Recorder PWR Screen Toggle Design

**Date:** 2026-07-12
**Target:** Waveshare ESP32-C6-Touch-AMOLED-2.16 Jinshan AI mode

## Goal

A short press of the board's AXP2101 PWR key turns the AMOLED off. The next
short press turns it on and redraws the current assistant state. Recording,
playback, Wi-Fi, WSS, queued turns, and SD persistence continue while dark.

## Chosen Direction

Three approaches were considered:

1. Disable the AXP2101 ALDO3 display rail. This saves the most display power,
   but waking requires panel reset and full SH8601/LVGL reinitialization.
2. Paint a black LVGL screen. This looks dark on AMOLED but continues refresh
   traffic, changes the active UI tree, and does not provide a real display-off
   state.
3. Stop LVGL and send SH8601 display on/off through the existing panel handle.
   Waking sends display-on, invalidates the active screen, and resumes LVGL.

Direction 3 is selected. It preserves the current UI tree, uses the panel API
already proven during initialization, and prevents background LVGL traffic from
contending with SD on shared SPI2 while the screen is off.

## Input and State Flow

Recorder initialization enables the AXP2101 IRQ2 short-press bit and clears
stale IRQ status. A small polling task reads the PMIC status over the existing
I2C bus. It never calls LVGL, the panel, SD, audio, or network code; it only
publishes `kPhysicalPower` into the existing bounded-by-consumption Recorder
request queue.

The Recorder main task reduces that event in every control mode. It toggles the
pure `screen_on` state and emits `kScreenPowerChanged`. The existing request
drain applies the display transition, including during recording and playback,
without changing the active voice/playback state.

## Display and SPI2 Ownership

`recorder_display.cc` retains the initialized SH8601 panel handle. Turning off
adds one persistent Recorder display pause, sends panel display-off, and keeps
LVGL stopped. Existing SD pauses may nest above this persistent pause and cannot
resume LVGL accidentally.

Turning on sends panel display-on while LVGL is still stopped and locked,
invalidates the active screen, then releases the persistent pause. LVGL resumes
and flushes the latest assistant or history state accumulated while dark.
Touch is inactive while LVGL is stopped; only another PWR short press wakes the
screen.

## Failure Handling

PMIC read failures are rate-limited in the serial log and recover without
restarting the app. A failed panel transition is logged and the pure control
state is restored to the last applied display state. Unsupported or unavailable
display hardware leaves voice and storage behavior operational.

Long PWR holds remain hardware-managed and are not repurposed. Startup clears
stale IRQ status so a press that occurred before Recorder became ready cannot
immediately blank the screen.

## Verification

Host tests prove:

- PWR toggles screen state in idle, recording, playing, and paused modes;
- PWR does not alter recording/playback mode, volume, or Agent phase;
- the existing AXP2101 short-press mask accepts IRQ2 bit `0x08` and ignores
  long-press-only status.

The full ESP-IDF build proves the SH8601 and LVGL integration. Real-device
acceptance presses PWR off/on twice, requires serial off/on milestones, confirms
WSS remains ready, and rejects SPI2 assertions, stack faults, Guru Meditation,
queue overflow, or reboot loops.

## Non-Goals

- Deep sleep, modem sleep, shutdown, or battery policy changes.
- PWR behavior changes in XiaoZhi or BLE keyboard modes.
- Touch-to-wake or automatic inactivity timeout.
- Reinitializing the display rail or deleting/recreating the LVGL object tree.
