# BLE 键盘与触控板模块开发指南

> 最后更新：2026-08-19
> 位置：`main/apps/ble_keyboard/`

## 1. 模块概述

本模块在不启动小智语音主应用的情况下，把 Waveshare ESP32-C6 Touch
AMOLED 2.16 作为单连接 BLE HID 键盘与相对鼠标。亮屏时 CST9217 全屏触摸
转换为鼠标位移，暗屏时相同坐标恢复九宫格键盘动作；AXP2101 PWR 短按在两种
输入状态间切换。模块还独立初始化 SH8601/LVGL、复用 SPI2 上的 SD 卡，并在
亮屏右上角显示 AXP2101 报告的剩余电量。

## 2. 核心代码结构

| 文件 | 职责 | 关键类 / 方法 |
| --- | --- | --- |
| `keyboard_app.cc` | 键鼠应用入口、BLE/显示/SD 初始化、物理左右键绑定和心跳 | `RunKeyboardApp()` |
| `ble_hid_keyboard.*` | Report ID 1 键盘、Report ID 2 相对鼠标、连接状态和非阻塞发送 | `BleHidKeyboard::Init()`、`TapKey()`、`SendMouseMove()`、`SendMouseButton()` |
| `esp_hid_gap.*` | Apache NimBLE GAP、持续广播、Just Works bonding 和主机驱动的稳定连接参数 | `esp_hid_gap_init()`、`esp_hid_ble_gap_adv_init()`、`esp_hid_ble_gap_adv_start()` |
| `keyboard_touch_arrows.*` | I2C0/PMIC/CST9217 初始化、50 Hz 轮询、模式切换、电量采样和 HID 调度 | `StartKeyboardTouchArrows()`、`TouchArrowTask()`、`ReadKeyboardTouch()`、`ReadAxp2101BatteryLevel()` |
| `keyboard_zone_display.*` | SPI2/SH8601/LVGL 独立初始化、亮灭屏、九宫格和电量标签 | `KeyboardInputModeDisplayToggle()`、`KeyboardZoneDisplaySetBatteryLevel()` |
| `keyboard_touch_action.*` | profile 与九宫格坐标到键盘动作的纯映射 | `MapTouchPointToKeyboardAction()` |
| `keyboard_pmic_power_key.*` | AXP2101 IRQ2 位解析和 PWR 行为选择 | `IsAxp2101PowerKeyToggleEvent()`、`PowerKeyShortPressTogglesDisplay()` |
| `touchpad_motion.*` | 绝对触点到有界相对鼠标位移、加速和跳点拒绝 | `TouchpadMotion::Update()`、`Reset()` |
| `mouse_move_accumulator.*` | BLE 暂时拥塞时有界累计相对位移，并按 HID `[-127,127]` 分片排空 | `Add()`、`PeekChunk()`、`Commit()`、`Reset()` |
| `keyboard_touch_gesture.*`、`touch_arrow_mapper.*` | MENU 热角和四向映射的纯逻辑 | `IsKeyboardSelectorHotCorner()`、`MapTouchPointToArrow()` |
| `*_test.cc` | 不依赖 ESP-IDF 硬件的主机回归 | 各文件 `main()` |

## 3. 核心业务流程

- **启动键鼠应用**：`RunKeyboardApp()` → `BleHidKeyboard::Init()` →
  `KeyboardZoneDisplayPrepareSharedSpiBus()` → `SdCardMount(false)` →
  `StartKeyboardTouchArrows()` → 建立物理按键回调并进入 5 秒心跳。
- **亮屏触控板**：`TouchArrowTask()` → 在 I2C 锁内
  `ReadKeyboardTouch()` 并完成 CST9217 ACK → 释放 I2C 锁 →
  `TouchpadMotion::Update()` → `BleHidKeyboard::SendMouseMove()`；即使当前
  触摸增量为零，也会尝试排空先前因通知拥塞暂存的位移。
- **暗屏键盘**：PWR IRQ → 释放 I2C 锁 →
  `KeyboardInputModeDisplayToggle()` 停止 LVGL 并关闭面板 →
  `MapTouchPointToKeyboardAction()` → `TapKey()` 或 `SendModifier()`。
- **电量显示**：启动及约每 10 秒在共享 I2C 锁内读取 AXP2101 `0xA4` →
  校验 `0..100` → 释放 I2C 锁 →
  `KeyboardZoneDisplaySetBatteryLevel()`；亮屏时更新右上角 16px
  Montserrat 百分比，暗屏时只缓存并在下次 `DrawZoneGuideLocked()` 重绘。
- **返回选择器**：暗屏时左上 `MENU` 区持续 2 秒 →
  `AppModeWriteAndReboot(AppMode::kSelector)`。

## 4. 关键资源与副作用

- BLE：设备名 `XiaoZhi KB`，VID/PID `0x16C0/0x05DF`，Report ID 1 为
  8 字节键盘，Report ID 2 为 4 字节相对鼠标；bond 存在 NVS。
- I2C0：GPIO8/GPIO7 上的 AXP2101 `0x34` 与 CST9217 `0x5A` 共用
  `TouchArrowContext::i2c_mutex`；触摸帧读取与 `0xAB` ACK 不得拆锁。
- SPI2：AMOLED 与 SD 共用 GPIO0/1/2，必须先按屏幕最大传输尺寸初始化总线，
  再让 `SdCardMount(false)` 复用。
- LVGL：PWR 息屏后 `PauseDisplayRefresh()` 持续持有显示锁；暗屏期间禁止提交
  UI 刷新。电量字体依赖根 `sdkconfig.defaults` 中
  `CONFIG_LV_FONT_MONTSERRAT_16=y`。
- GPIO：BOOT/GPIO9 为鼠标左键，GPIO10 为鼠标右键；GPIO12/13 是 USB，
  不得用于按键。
- 鼠标通知：发送失败后保持至少 100 ms 退避；退避期间继续把位移累计到
  `MouseMoveAccumulator`，单轴最多保留 1024，相反方向可抵消，恢复后每个
  触摸 tick 最多发送一个 HID 分片。断连和亮暗屏切换会清除残留位移。
- 连接参数：CONNECT 事件只发起安全流程，不同时主动发起本地参数更新；macOS
  先完成自己的 15 ms HID 更新，`CONN_UPDATE` 完成事件若仍显示低于 4 秒的
  supervision timeout，外设才以相同 interval 跟进请求 6 秒。`CONN_UPDATE_REQ`
  约束仅作可协商路径的提前保护。禁止恢复 CONNECT 中并发
  `ble_gap_update_params()`，否则会与加密/主机更新发生 HCI `0x2a` transaction
  collision。

## 5. 常见修改场景与切入点

- 调整电量字号、颜色或位置：修改 `DrawZoneGuideLocked()`；新增 LVGL 内置
  字号时同步更新 `sdkconfig.defaults`，并执行完整 `idf.py build`。
- 调整电量刷新周期：修改 `kBatteryPollMs`；PMIC 读取必须留在 I2C 锁内，
  `KeyboardZoneDisplaySetBatteryLevel()` 必须在释放锁后调用。
- 修改九宫格键位：先改 `MapTouchPointToKeyboardAction()` 及其测试，再核对
  `DrawZoneGuideLocked()` 的标签与 `TouchArrowTask()` 的按住/重复逻辑。
- 调整鼠标手感：修改 `TouchpadMotionConfig`，运行
  `touchpad_motion_test.cc`，真机检查慢速精细移动、快速加速、抬手重按和跳点；
  修改拥塞处理时还要运行 `mouse_move_accumulator_test.cc`，并观察串口
  `mouse transport` 的 failed/coalesced/pending 统计。
- 修改 BLE report map 或配对：从 `kKeyboardMouseReportMap`、
  `s_hid_config` 和 `esp_hid_gap.c` 入手；真机前让 macOS 忽略旧设备并重新配对。
- 修改 PWR 行为：同时检查 `keyboard_pmic_power_key.*`、
  `PollPowerKeyShortcut()`、显示 pause/resume 和跨模式触点抑制。

## 6. 维护与风险说明

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| 持 I2C 锁执行 LVGL 或 BLE HID | 中 | 高：50 Hz 输入停顿、监督超时或死锁 | I2C 事务完成后立即解锁，再做显示/HID 工作 |
| 息屏仍刷新 LVGL | 中 | 高：面板关闭时提交 SPI2 帧、与 SD 争用或触发断言 | 暗屏只缓存电量，保持 `PauseDisplayRefresh()` 锁直到亮屏 |
| SD 先创建 4 KiB SPI 总线 | 低 | 高：AMOLED 刷新传输失败 | 始终先调用 `KeyboardZoneDisplayPrepareSharedSpiBus()` |
| 改 HID 描述符但沿用主机缓存 | 高 | 中：macOS 仍识别为旧键盘 | 忽略 `XiaoZhi KB` 后重新配对 |
| 未编入新 LVGL 字体 | 中 | 中：编译期字体符号缺失 | 在 `sdkconfig.defaults` 固化字体并执行干净/完整构建 |
| BLE 通知暂时拥塞后直接丢位移 | 中 | 中：长时间拖动逐渐跳顿 | 有界合并位移、100 ms 后分片排空，并用低频统计确认积压归零 |

主机纯逻辑测试命令集中在根 `README.md` 的“测试”章节。涉及显示、I2C、
BLE 或 SPI2 的修改除主机测试和 `idf.py build` 外，还必须真机检查
`keyboard heartbeat`、BLE 重连、PWR 两次切换和无 SPI/栈/重启故障。
