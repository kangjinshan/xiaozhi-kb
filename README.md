# XiaoZhi KB：小智语音 + 蓝牙 HID 键盘固件

这是一个基于小智 ESP32 开源工程改造的 ESP32-C6 固件，目标硬件是 **Waveshare ESP32-C6-Touch-AMOLED-2.16**。它把同一块开发板做成两个可切换的应用：

- **小智语音模式**：保留小智原有语音助手能力。
- **蓝牙键盘模式**：作为 BLE HID 键盘连接 MacBook 等主机，支持实体键和触屏方向键。
- **开机选择界面**：当未选择模式或主动返回选择界面时，通过触摸屏选择进入小智语音或蓝牙键盘。

项目当前已在真机上验证：蓝牙键盘可连接、实体按键可识别、触屏方向键可用、PWR 退格无连发。

## 硬件目标

当前主要适配：

- 开发板：Waveshare ESP32-C6-Touch-AMOLED-2.16
- MCU：ESP32-C6
- 屏幕：2.16 英寸 AMOLED，480 x 480
- 触摸：CST9217，I2C
- 电源管理：AXP2101
- 蓝牙：BLE HID 键盘
- 串口：USB-Serial/JTAG，通常为 `/dev/cu.usbmodem1101`

重要硬件约束：

- ESP32-C6 只支持 BLE，不支持 Classic Bluetooth，所以键盘走 BLE HID。
- GPIO12 / GPIO13 是 USB D- / D+，不能改成普通 GPIO。
- 板上三个实体键的实际用途为：

| 位置 | 丝印 | 实际用途 | 当前固件行为 |
| --- | --- | --- | --- |
| 最左 | IO10 | GPIO10 普通按键 | 蓝牙键盘模式下按住为右 Option |
| 中间 | PWR | AXP2101 电源键 | 蓝牙键盘模式下单击为退格，长按仍可能触发电源行为 |
| 最右 | BOOT | GPIO9 / BOOT 键 | 蓝牙键盘模式下单击为确定/回车 |

## 功能说明

### 1. 开机模式分派

固件启动后会读取 NVS：

- namespace：`appsel`
- key：`mode`
- 可选值：`selector`、`xiaozhi`、`keyboard`

分派规则：

- `mode=keyboard`：进入蓝牙键盘模式。
- `mode=xiaozhi`：进入小智语音模式。
- NVS 为空、值非法或 `mode=selector`：进入开机选择界面。

相关代码：

- `main/main.cc`
- `main/apps/app_mode.cc`
- `main/apps/app_selector.cc`

### 2. 开机选择界面

选择界面直接运行在板载触摸屏上，显示两个入口：

- 小智语音
- 蓝牙键盘

点击后会写入 NVS 并软重启，重启后进入对应模式。选择界面不启动 Wi-Fi、音频或 BLE，只初始化显示和触摸，启动成本较低。

### 3. 蓝牙键盘模式

蓝牙键盘模式会广播 BLE HID 设备：

- 设备名：`XiaoZhi KB`
- HID 类型：Keyboard
- VID / PID：`0x16C0 / 0x05DF`
- 配对方式：Just Works，无 PIN

当前键位：

| 输入 | 输出 |
| --- | --- |
| 最左键 GPIO10 按住 | 右 Option / Right Alt |
| BOOT 单击 | 确定 / 回车 |
| PWR 单击 | 退格 |
| 触摸屏上 / 下 / 左 / 右区域 | 键盘方向键上 / 下 / 左 / 右 |
| 触摸屏左上角长按 2 秒 | 返回开机选择界面 |

触屏方向键支持按住重复发送，当前重复间隔约 120 ms。

相关代码：

- `main/apps/ble_keyboard/keyboard_app.cc`
- `main/apps/ble_keyboard/ble_hid_keyboard.cc`
- `main/apps/ble_keyboard/keyboard_touch_arrows.cc`
- `main/apps/ble_keyboard/touch_arrow_mapper.cc`
- `main/apps/ble_keyboard/keyboard_touch_gesture.cc`
- `main/apps/ble_keyboard/keyboard_pmic_power_key.cc`

### 4. 小智语音模式

小智语音模式沿用原项目的 `Application` 主流程：

- 设备初始化
- 显示和音频
- 网络与协议
- 小智主循环

在小智语音模式下，BOOT 键长按 2 秒会写入 `mode=selector` 并重启，返回开机选择界面。

## 目录结构

项目关键目录如下：

```text
.
├── main/
│   ├── main.cc                         # 固件入口，按 NVS 模式分派应用
│   ├── apps/
│   │   ├── app_mode.*                  # 模式读写和重启
│   │   ├── app_selector.*              # 触屏开机选择界面
│   │   └── ble_keyboard/               # BLE HID 键盘应用
│   ├── boards/                         # 各硬件板级适配
│   ├── display/                        # 显示抽象和 LVGL 显示实现
│   ├── protocols/                      # 小智协议实现
│   └── assets/                         # 语音、图片、语言资源
├── partitions/                         # 分区表
├── scripts/                            # 构建、资源、诊断辅助脚本
├── sdkconfig.defaults.esp32c6          # ESP32-C6 默认配置
└── CMakeLists.txt
```

## 开发环境

已验证环境：

- ESP-IDF 5.5
- 目标芯片：ESP32-C6
- macOS 开发机

初始化环境：

```bash
source ~/esp/esp-idf/export.sh
cd ~/xiaozhi-kb
idf.py set-target esp32c6
```

## 构建与烧录

完整构建：

```bash
source ~/esp/esp-idf/export.sh
cd ~/xiaozhi-kb
idf.py build
```

烧录到真机：

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

串口监听：

```bash
idf.py -p /dev/cu.usbmodem1101 monitor
```

如果端口不同，先查看当前 USB 串口：

```bash
ls /dev/cu.usbmodem*
```

## 首次使用

1. 编译并烧录固件。
2. 设备启动后，如果 NVS 里没有有效模式，会显示选择界面。
3. 在屏幕上点击「蓝牙键盘」。
4. 设备重启后进入蓝牙键盘模式。
5. 在 macOS 蓝牙设置中连接 `XiaoZhi KB`。
6. 连接成功后测试：
   - 最左键按住：右 Option。
   - BOOT 单击：回车。
   - PWR 单击：退格。
   - 触屏上下左右：方向键。

## 模式切换

从选择界面进入应用：

- 点「小智语音」进入小智模式。
- 点「蓝牙键盘」进入键盘模式。

从蓝牙键盘模式返回选择界面：

- 触摸屏左上角长按 2 秒。

从小智语音模式返回选择界面：

- BOOT 键长按 2 秒。

返回选择界面后，再点击另一个应用即可切换模式。

## 测试

仓库内包含几个可在主机上直接编译运行的小测试，用于验证不依赖硬件的纯逻辑：

```bash
c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/touch_arrow_mapper_test.cc \
  main/apps/ble_keyboard/touch_arrow_mapper.cc \
  -o /tmp/touch_arrow_mapper_test && /tmp/touch_arrow_mapper_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/keyboard_touch_gesture_test.cc \
  main/apps/ble_keyboard/keyboard_touch_gesture.cc \
  -o /tmp/keyboard_touch_gesture_test && /tmp/keyboard_touch_gesture_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/ble_keyboard \
  main/apps/ble_keyboard/keyboard_pmic_power_key_test.cc \
  main/apps/ble_keyboard/keyboard_pmic_power_key.cc \
  -o /tmp/keyboard_pmic_power_key_test && /tmp/keyboard_pmic_power_key_test
```

完整固件验证仍以 `idf.py build` 和真机烧录测试为准。

## 常见问题

### 1. 为什么键盘只做 BLE HID？

ESP32-C6 没有 Classic Bluetooth，只能使用 BLE。当前实现基于 ESP-IDF 的 `esp_hid` BLE HID 能力。

### 2. 为什么 PWR 键要特殊处理？

PWR 不是普通 GPIO，而是接在 AXP2101 PMIC 上的电源键。固件通过读取 AXP2101 短按中断来识别单击，并映射为退格。长按 PWR 仍可能触发硬件电源行为，调试和日常使用时不要把它当成长按功能键使用。

### 3. 为什么自动复位后看起来没有正常启动？

这块板在某些 DTR/RTS 自动复位序列下可能停在 ROM download/stub 状态。遇到黑屏、无日志或端口异常时，优先做真正断电冷启动：

```text
拔 USB -> 拔锂电池 -> 等 20 秒 -> 不按 BOOT -> 重新插 USB / 接电
```

### 4. 如何进入下载模式？

```text
拔 USB -> 拔锂电池 -> 等 20 秒 -> 按住 BOOT -> 插 USB -> 保持 BOOT 约 3 秒
```

### 5. 为什么不能动 GPIO12 / GPIO13？

GPIO12 和 GPIO13 是 ESP32-C6 的 USB D- / D+。把它们配置成普通 GPIO 会破坏 USB 通信，可能导致串口消失。

## 当前状态

已完成并验证：

- 开机选择界面。
- NVS 模式保存和启动分派。
- 小智语音模式入口。
- 蓝牙 HID 键盘广播和连接。
- GPIO10 右 Option。
- BOOT 单击回车。
- PWR 单击退格，且真机验证无连发。
- 触屏方向键。
- 蓝牙键盘模式下触屏左上角长按返回选择界面。
- 小智模式下 BOOT 长按返回选择界面。

仍需注意：

- 本项目当前主要面向 Waveshare ESP32-C6-Touch-AMOLED-2.16；其他板型可能需要调整板级配置、触摸方向和电源管理逻辑。
- 键盘模式没有完整 UI，屏幕主要用于触摸方向键和返回选择界面。
- 若修改分区、OTA 或 NVS 逻辑，务必同时验证冷启动和模式切换。

## 许可

本仓库基于上游小智 ESP32 工程改造，保留原项目许可文件。新增改动遵循仓库现有许可约定。
