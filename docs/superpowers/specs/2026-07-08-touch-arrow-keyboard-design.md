# Touch Arrow Keyboard Design

## Goal

Add touch-screen directional input to the BLE keyboard app on the Waveshare ESP32-C6 Touch AMOLED 2.16 board.

When the user touches and holds the screen, the keyboard app should continuously send the corresponding BLE HID arrow key. Releasing the touch stops the repeated arrow key output.

## Current Context

The board-specific Xiaozhi app already initializes the CST9217 touch controller through I2C and attaches it to LVGL. The BLE keyboard app currently bypasses the normal board/display application path and initializes only BLE HID plus two physical buttons.

Because keyboard mode does not initialize LVGL or the display, touch support should be added as a small keyboard-mode subsystem rather than by starting the full Xiaozhi UI stack.

Relevant hardware and code facts:

- Touch controller: CST9217 over I2C.
- I2C pins: `AUDIO_CODEC_I2C_SDA_PIN` and `AUDIO_CODEC_I2C_SCL_PIN`.
- Touch pins: `TP_RST_GPIO` and `TP_INT_GPIO`.
- Screen size constants: `LCD_H_RES` and `LCD_V_RES`.
- Existing BLE keyboard wrapper can send keyboard reports through `BleHidKeyboard::TapKey()`.
- The middle PWR button is not part of this feature.

## Behavior

Touch input is continuous:

- Press and hold upper screen area: repeatedly send Arrow Up.
- Press and hold lower screen area: repeatedly send Arrow Down.
- Press and hold left screen area: repeatedly send Arrow Left.
- Press and hold right screen area: repeatedly send Arrow Right.
- Release the touch: stop sending arrow keys.
- Moving the finger to a different directional area changes the repeated key on the next poll/repeat cycle.

The repeat interval should start at 120 ms. This is fast enough to feel like key-repeat but slow enough to avoid flooding the BLE HID path.

## Direction Mapping

Use center-relative dominant-axis mapping:

1. Compute the touch point offset from the screen center.
2. If horizontal offset magnitude is larger than vertical offset magnitude, map to Left or Right.
3. Otherwise map to Up or Down.

This makes the entire screen usable and avoids needing visible UI regions. It also behaves naturally on a square 480 x 480 display.

## Architecture

Add a small touch-arrow module under `main/apps/ble_keyboard/`.

Suggested units:

- `touch_arrow_mapper.h/.cc`
  - Pure coordinate-to-direction logic.
  - No ESP-IDF dependencies, so it can be host-tested.
- `keyboard_touch_arrows.h/.cc`
  - ESP-IDF hardware integration for keyboard mode.
  - Initializes I2C, minimal AXP2101 power setup needed by the board, and CST9217.
  - Starts a FreeRTOS polling task.
  - Converts touch coordinates to HID arrow keycodes and calls `BleHidKeyboard::TapKey()`.

`keyboard_app.cc` should call the touch-arrow startup after `BleHidKeyboard::Init()` and continue to keep the existing two physical button bindings.

## Error Handling

Touch support should fail soft:

- If I2C, PMIC, or CST9217 initialization fails, log the error and keep BLE keyboard mode running.
- Physical buttons must continue to work even when touch initialization fails.
- Touch polling errors should be logged at a controlled rate and should not restart the device.

## HID Keycodes

Add standard USB HID keyboard usages for the arrow keys:

- Arrow Right: `0x4F`
- Arrow Left: `0x50`
- Arrow Down: `0x51`
- Arrow Up: `0x52`

## Tests And Verification

Use TDD for the pure mapping behavior:

1. Add a host-buildable C++ test for `touch_arrow_mapper`.
2. Verify the test fails before implementing the mapping.
3. Implement the mapping and verify the test passes.

Firmware verification:

1. Build the ESP32-C6 firmware with `idf.py build`.
2. Flash the device.
3. Confirm BLE keyboard still connects as `XiaoZhi KB`.
4. Confirm existing physical buttons still work.
5. Confirm touch-and-hold repeats each arrow key and release stops repetition.

Because touch behavior depends on physical hardware, final input verification requires testing on the actual device after flashing.

## Out Of Scope

- Drawing visible direction buttons on the display.
- Starting the full LVGL display UI in keyboard mode.
- Multi-touch gestures.
- Configurable repeat rate in UI.
- Changing the existing physical button behavior.
