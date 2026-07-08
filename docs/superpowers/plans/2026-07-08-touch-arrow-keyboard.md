# Touch Arrow Keyboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add touch-and-hold directional arrow key repeat to the BLE keyboard app on the Waveshare ESP32-C6 Touch AMOLED 2.16 board.

**Architecture:** Keep coordinate mapping separate from hardware polling. Add a pure `touch_arrow_mapper` unit that maps touch coordinates to directions and can be tested on the host, then add a keyboard-mode `keyboard_touch_arrows` FreeRTOS task that initializes CST9217 touch and repeatedly sends BLE HID arrow key taps while the screen is held.

**Tech Stack:** ESP-IDF C++, FreeRTOS, BLE HID keyboard reports, CST9217 via `esp_lcd_touch_cst9217`, I2C master driver, host C++ test compiled with `c++`.

---

## File Structure

- Create `main/apps/ble_keyboard/touch_arrow_mapper.h`
  - Declares `TouchArrowDirection`, `MapTouchPointToArrow()`, and `TouchArrowDirectionName()`.
- Create `main/apps/ble_keyboard/touch_arrow_mapper.cc`
  - Implements pure center-relative dominant-axis coordinate mapping.
- Create `main/apps/ble_keyboard/touch_arrow_mapper_test.cc`
  - Host-buildable assertion test for the mapping behavior. This file is not added to ESP-IDF firmware sources.
- Modify `main/apps/ble_keyboard/ble_hid_keyboard.h`
  - Adds HID arrow key usage constants.
- Create `main/apps/ble_keyboard/keyboard_touch_arrows.h`
  - Declares the keyboard-mode touch-arrow startup function.
- Create `main/apps/ble_keyboard/keyboard_touch_arrows.cc`
  - Initializes keyboard-mode I2C, minimal PMIC power rails, CST9217 touch, and the FreeRTOS polling task.
- Modify `main/apps/ble_keyboard/keyboard_app.cc`
  - Starts touch arrows after BLE HID initialization while preserving existing physical button behavior.
- Modify `main/CMakeLists.txt`
  - Adds the new firmware source files to `SOURCES`.

## Task 1: Write The Failing Mapper Test

**Files:**
- Create: `main/apps/ble_keyboard/touch_arrow_mapper_test.cc`

- [ ] **Step 1: Add the host test**

Create `main/apps/ble_keyboard/touch_arrow_mapper_test.cc` with this content:

```cpp
#include "touch_arrow_mapper.h"

#include <cstdio>
#include <cstdlib>

static void ExpectDirection(const char* name,
                            TouchArrowDirection actual,
                            TouchArrowDirection expected) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: expected %s, got %s\n",
                     name,
                     TouchArrowDirectionName(expected),
                     TouchArrowDirectionName(actual));
        std::exit(1);
    }
}

int main() {
    constexpr uint16_t kWidth = 480;
    constexpr uint16_t kHeight = 480;

    ExpectDirection("top center maps to up",
                    MapTouchPointToArrow(240, 10, kWidth, kHeight),
                    TouchArrowDirection::kUp);
    ExpectDirection("bottom center maps to down",
                    MapTouchPointToArrow(240, 470, kWidth, kHeight),
                    TouchArrowDirection::kDown);
    ExpectDirection("left center maps to left",
                    MapTouchPointToArrow(10, 240, kWidth, kHeight),
                    TouchArrowDirection::kLeft);
    ExpectDirection("right center maps to right",
                    MapTouchPointToArrow(470, 240, kWidth, kHeight),
                    TouchArrowDirection::kRight);
    ExpectDirection("dominant horizontal offset wins",
                    MapTouchPointToArrow(20, 200, kWidth, kHeight),
                    TouchArrowDirection::kLeft);
    ExpectDirection("dominant vertical offset wins",
                    MapTouchPointToArrow(220, 20, kWidth, kHeight),
                    TouchArrowDirection::kUp);
    ExpectDirection("zero width is invalid",
                    MapTouchPointToArrow(10, 10, 0, kHeight),
                    TouchArrowDirection::kNone);
    ExpectDirection("zero height is invalid",
                    MapTouchPointToArrow(10, 10, kWidth, 0),
                    TouchArrowDirection::kNone);

    return 0;
}
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/touch_arrow_mapper_test.cc \
  main/apps/ble_keyboard/touch_arrow_mapper.cc \
  -o /tmp/touch_arrow_mapper_test
```

Expected result: compile fails because `touch_arrow_mapper.h` and `touch_arrow_mapper.cc` do not exist yet.

## Task 2: Implement The Mapper

**Files:**
- Create: `main/apps/ble_keyboard/touch_arrow_mapper.h`
- Create: `main/apps/ble_keyboard/touch_arrow_mapper.cc`
- Test: `main/apps/ble_keyboard/touch_arrow_mapper_test.cc`

- [ ] **Step 1: Add the mapper header**

Create `main/apps/ble_keyboard/touch_arrow_mapper.h`:

```cpp
#ifndef TOUCH_ARROW_MAPPER_H_
#define TOUCH_ARROW_MAPPER_H_

#include <cstdint>

enum class TouchArrowDirection : uint8_t {
    kNone = 0,
    kUp,
    kDown,
    kLeft,
    kRight,
};

TouchArrowDirection MapTouchPointToArrow(uint16_t x,
                                         uint16_t y,
                                         uint16_t width,
                                         uint16_t height);

const char* TouchArrowDirectionName(TouchArrowDirection direction);

#endif  // TOUCH_ARROW_MAPPER_H_
```

- [ ] **Step 2: Add the mapper implementation**

Create `main/apps/ble_keyboard/touch_arrow_mapper.cc`:

```cpp
#include "touch_arrow_mapper.h"

#include <cstdlib>

TouchArrowDirection MapTouchPointToArrow(uint16_t x,
                                         uint16_t y,
                                         uint16_t width,
                                         uint16_t height) {
    if (width == 0 || height == 0) {
        return TouchArrowDirection::kNone;
    }

    const int32_t center_x = static_cast<int32_t>(width) / 2;
    const int32_t center_y = static_cast<int32_t>(height) / 2;
    const int32_t dx = static_cast<int32_t>(x) - center_x;
    const int32_t dy = static_cast<int32_t>(y) - center_y;

    if (std::abs(dx) > std::abs(dy)) {
        return dx < 0 ? TouchArrowDirection::kLeft : TouchArrowDirection::kRight;
    }

    return dy < 0 ? TouchArrowDirection::kUp : TouchArrowDirection::kDown;
}

const char* TouchArrowDirectionName(TouchArrowDirection direction) {
    switch (direction) {
        case TouchArrowDirection::kUp:
            return "up";
        case TouchArrowDirection::kDown:
            return "down";
        case TouchArrowDirection::kLeft:
            return "left";
        case TouchArrowDirection::kRight:
            return "right";
        case TouchArrowDirection::kNone:
        default:
            return "none";
    }
}
```

- [ ] **Step 3: Run the mapper test to verify GREEN**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/touch_arrow_mapper_test.cc \
  main/apps/ble_keyboard/touch_arrow_mapper.cc \
  -o /tmp/touch_arrow_mapper_test && /tmp/touch_arrow_mapper_test
```

Expected result: command exits with status 0 and prints no failure lines.

- [ ] **Step 4: Commit mapper work**

Run:

```bash
git add main/apps/ble_keyboard/touch_arrow_mapper.h \
        main/apps/ble_keyboard/touch_arrow_mapper.cc \
        main/apps/ble_keyboard/touch_arrow_mapper_test.cc
git commit -m "feat(keyboard): add touch arrow mapper"
```

## Task 3: Add HID Arrow Constants

**Files:**
- Modify: `main/apps/ble_keyboard/ble_hid_keyboard.h`

- [ ] **Step 1: Add arrow key usage constants**

In `main/apps/ble_keyboard/ble_hid_keyboard.h`, extend the HID keycode block to include:

```cpp
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT  0x50
#define HID_KEY_ARROW_DOWN  0x51
#define HID_KEY_ARROW_UP    0x52
```

The block should read:

```cpp
// USB HID modifier bits / keycodes（与官方 USB HID Usage Tables 一致）
#define HID_MOD_RIGHT_ALT 0x40   // 右 Option (Right Alt/Option)
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_ENTER     0x28
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT  0x50
#define HID_KEY_ARROW_DOWN  0x51
#define HID_KEY_ARROW_UP    0x52
```

- [ ] **Step 2: Commit HID constants**

Run:

```bash
git add main/apps/ble_keyboard/ble_hid_keyboard.h
git commit -m "feat(keyboard): add HID arrow key constants"
```

## Task 4: Add Keyboard Touch Arrow Hardware Module

**Files:**
- Create: `main/apps/ble_keyboard/keyboard_touch_arrows.h`
- Create: `main/apps/ble_keyboard/keyboard_touch_arrows.cc`

- [ ] **Step 1: Add the module header**

Create `main/apps/ble_keyboard/keyboard_touch_arrows.h`:

```cpp
#ifndef KEYBOARD_TOUCH_ARROWS_H_
#define KEYBOARD_TOUCH_ARROWS_H_

class BleHidKeyboard;

void StartKeyboardTouchArrows(BleHidKeyboard& keyboard);

#endif  // KEYBOARD_TOUCH_ARROWS_H_
```

- [ ] **Step 2: Add the hardware implementation**

Create `main/apps/ble_keyboard/keyboard_touch_arrows.cc`:

```cpp
#include "keyboard_touch_arrows.h"

#include "ble_hid_keyboard.h"
#include "config.h"
#include "touch_arrow_mapper.h"

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst9217.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "kb_touch_arrows"

namespace {

constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kRepeatMs = 120;
constexpr uint8_t kAxp2101Address = 0x34;

struct TouchArrowContext {
    BleHidKeyboard* keyboard;
    esp_lcd_touch_handle_t touch;
};

uint8_t HidKeyForDirection(TouchArrowDirection direction) {
    switch (direction) {
        case TouchArrowDirection::kUp:
            return HID_KEY_ARROW_UP;
        case TouchArrowDirection::kDown:
            return HID_KEY_ARROW_DOWN;
        case TouchArrowDirection::kLeft:
            return HID_KEY_ARROW_LEFT;
        case TouchArrowDirection::kRight:
            return HID_KEY_ARROW_RIGHT;
        case TouchArrowDirection::kNone:
        default:
            return 0;
    }
}

esp_err_t WriteI2cReg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    const uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(device, buffer, sizeof(buffer), 100);
}

esp_err_t InitializeKeyboardPmic(i2c_master_bus_handle_t i2c_bus) {
    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAxp2101Address,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    i2c_master_dev_handle_t pmic = nullptr;
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &device_cfg, &pmic);
    if (err != ESP_OK) {
        return err;
    }

    const struct {
        uint8_t reg;
        uint8_t value;
    } writes[] = {
        {0x22, 0b00000110},
        {0x27, 0x10},
        {0x80, 0x01},
        {0x90, 0x00},
        {0x91, 0x00},
        {0x82, static_cast<uint8_t>((3300 - 1500) / 100)},
        {0x92, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x93, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x94, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x95, static_cast<uint8_t>((3300 - 500) / 100)},
        {0x90, 0x0F},
        {0x64, 0x02},
        {0x61, 0x02},
        {0x62, 0x0A},
        {0x63, 0x01},
    };

    for (const auto& write : writes) {
        err = WriteI2cReg(pmic, write.reg, write.value);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t InitializeTouch(esp_lcd_touch_handle_t* out_touch) {
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    i2c_master_bus_handle_t i2c_bus = nullptr;
    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    err = InitializeKeyboardPmic(i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_io_handle_t touch_io = nullptr;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    touch_io_config.scl_speed_hz = 400 * 1000;
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &touch_io_config, &touch_io);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES - 1,
        .y_max = LCD_V_RES - 1,
        .rst_gpio_num = TP_RST_GPIO,
        .int_gpio_num = TP_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    return esp_lcd_touch_new_i2c_cst9217(touch_io, &touch_cfg, out_touch);
}

void TouchArrowTask(void* arg) {
    auto* context = static_cast<TouchArrowContext*>(arg);
    TouchArrowDirection last_direction = TouchArrowDirection::kNone;
    TickType_t last_send_tick = 0;

    while (true) {
        TouchArrowDirection direction = TouchArrowDirection::kNone;
        esp_err_t err = esp_lcd_touch_read_data(context->touch);
        if (err == ESP_OK) {
            uint16_t x = 0;
            uint16_t y = 0;
            uint8_t touch_count = 0;
            const bool pressed = esp_lcd_touch_get_coordinates(
                context->touch, &x, &y, nullptr, &touch_count, 1);
            if (pressed && touch_count > 0) {
                direction = MapTouchPointToArrow(x, y, LCD_H_RES, LCD_V_RES);
            }
        } else {
            ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
        }

        const TickType_t now = xTaskGetTickCount();
        if (direction == TouchArrowDirection::kNone) {
            last_direction = TouchArrowDirection::kNone;
            last_send_tick = 0;
        } else if (direction != last_direction ||
                   last_send_tick == 0 ||
                   now - last_send_tick >= pdMS_TO_TICKS(kRepeatMs)) {
            const uint8_t hid_key = HidKeyForDirection(direction);
            if (hid_key != 0 && context->keyboard->IsConnected()) {
                ESP_LOGI(TAG, "touch arrow %s", TouchArrowDirectionName(direction));
                context->keyboard->TapKey(hid_key);
            }
            last_direction = direction;
            last_send_tick = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
    }
}

}  // namespace

void StartKeyboardTouchArrows(BleHidKeyboard& keyboard) {
    static bool started = false;
    static TouchArrowContext context = {};

    if (started) {
        ESP_LOGW(TAG, "touch arrows already started");
        return;
    }

    esp_lcd_touch_handle_t touch = nullptr;
    esp_err_t err = InitializeTouch(&touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch arrows disabled: %s", esp_err_to_name(err));
        return;
    }

    context.keyboard = &keyboard;
    context.touch = touch;

    BaseType_t task_ok = xTaskCreate(
        TouchArrowTask, "touch_arrows", 4096, &context, 5, nullptr);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch arrow task");
        return;
    }

    started = true;
    ESP_LOGI(TAG, "touch arrows started");
}
```

- [ ] **Step 3: Commit hardware module**

Run:

```bash
git add main/apps/ble_keyboard/keyboard_touch_arrows.h \
        main/apps/ble_keyboard/keyboard_touch_arrows.cc
git commit -m "feat(keyboard): add touch arrow polling task"
```

## Task 5: Wire The Touch Module Into Keyboard Mode

**Files:**
- Modify: `main/apps/ble_keyboard/keyboard_app.cc`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add firmware sources to CMake**

In `main/CMakeLists.txt`, update the BLE keyboard source block to include:

```cmake
list(APPEND SOURCES "apps/ble_keyboard/ble_hid_keyboard.cc")
list(APPEND SOURCES "apps/ble_keyboard/esp_hid_gap.c")
list(APPEND SOURCES "apps/ble_keyboard/touch_arrow_mapper.cc")
list(APPEND SOURCES "apps/ble_keyboard/keyboard_touch_arrows.cc")
```

Keep `touch_arrow_mapper_test.cc` out of `SOURCES`.

- [ ] **Step 2: Include and start touch arrows in keyboard app**

In `main/apps/ble_keyboard/keyboard_app.cc`, add:

```cpp
#include "keyboard_touch_arrows.h"
```

After:

```cpp
kb.Init();
```

add:

```cpp
    StartKeyboardTouchArrows(kb);
```

Update the running log line to:

```cpp
    ESP_LOGI(TAG, "keyboard app running (LEFT/GPIO10=RightOption, RIGHT/GPIO9=Enter, touch=Arrow keys)");
```

- [ ] **Step 3: Commit integration**

Run:

```bash
git add main/CMakeLists.txt main/apps/ble_keyboard/keyboard_app.cc
git commit -m "feat(keyboard): start touch arrows in keyboard mode"
```

## Task 6: Verify Host Test And Firmware Build

**Files:**
- Test: `main/apps/ble_keyboard/touch_arrow_mapper_test.cc`
- Build: ESP-IDF project

- [ ] **Step 1: Re-run host mapper test**

Run:

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/touch_arrow_mapper_test.cc \
  main/apps/ble_keyboard/touch_arrow_mapper.cc \
  -o /tmp/touch_arrow_mapper_test && /tmp/touch_arrow_mapper_test
```

Expected result: command exits with status 0 and prints no failure lines.

- [ ] **Step 2: Build firmware**

Run:

```bash
. /Users/kanayama/esp/esp-idf/export.sh >/tmp/esp-idf-export.log 2>&1
idf.py build
```

Expected result: build succeeds and produces the ESP32-C6 app binary.

- [ ] **Step 3: Fix build-only issues if any**

If the build fails on ESP-IDF type names or initializer ordering, adjust only the new touch-arrow files and re-run:

```bash
idf.py build
```

Expected result: build succeeds.

## Task 7: Device Verification

**Files:**
- Firmware image from Task 6

- [ ] **Step 1: Flash the firmware**

Run:

```bash
. /Users/kanayama/esp/esp-idf/export.sh >/tmp/esp-idf-export.log 2>&1
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Expected result: firmware flashes successfully and logs show keyboard mode starts.

- [ ] **Step 2: Verify BLE keyboard still works**

Confirm on the Mac:

- Bluetooth device name remains `XiaoZhi KB`.
- Left physical key still acts as Right Option.
- Right physical key still acts as Enter.

- [ ] **Step 3: Verify touch repeat**

On a text field or key-test page:

- Hold upper screen area: Up repeats until release.
- Hold lower screen area: Down repeats until release.
- Hold left screen area: Left repeats until release.
- Hold right screen area: Right repeats until release.
- Move between directional areas while held: repeated key changes direction.

- [ ] **Step 4: Record result in the diagnosis handoff**

Append the final verification result to:

```text
/Users/kanayama/Desktop/AI/xiaozhi/诊断交接文档-蓝牙键盘.md
```

Include:

```markdown
## 触屏方向键验证

- 固件构建：通过
- BLE 键盘连接：通过
- 实体按键回归：通过
- 触屏方向键：按住连续发送，上/下/左/右松手停止
- 备注：触摸方向使用中心偏向判定，重复间隔 120ms
```
