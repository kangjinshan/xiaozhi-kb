# App Selector Implementation Plan

**Goal:** Implement the boot app selector described in section 9 of `/Users/kanayama/Desktop/AI/xiaozhi/诊断交接文档-蓝牙键盘.md`.

**Scope:** When `appsel/mode` is missing, invalid, or `selector`, boot into a lightweight LVGL touch selector. Selecting an app writes the NVS mode and reboots. Keyboard mode also needs one reachable return path to the selector.

## Tasks

- [x] Add a host-testable helper for the keyboard selector hot corner.
- [x] Add `RunAppSelector()` in `main/apps/app_selector.h/.cc`.
- [x] Update `main/main.cc` to dispatch `AppMode::kSelector` to the selector app.
- [x] Add selector and gesture sources to `main/CMakeLists.txt`.
- [x] Wire keyboard touch polling so holding the top-left hot corner for 2 seconds writes `mode=selector` and reboots.
- [x] Verify with host tests and an ESP-IDF build.

## Design Notes

- The selector initializes `Board::GetInstance()` and uses the existing LVGL display/touch stack, but does not call `Application::Initialize()`, so WiFi/audio/BLE services are not started by the selector.
- The selector UI uses runtime display dimensions instead of assuming the handoff document's 480x480 note, because the active board config reports `DISPLAY_WIDTH=410` and `DISPLAY_HEIGHT=502`.
- Keyboard return gesture uses a small top-left hot corner with a 2-second hold to avoid conflicting with normal arrow taps.
