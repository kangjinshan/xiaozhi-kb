# XiaoZhi KB 协作开发指南

> 最后更新：2026-07-11

## 1. 系统概述

本仓库基于小智 ESP32 开源固件，主要适配 `Waveshare ESP32-C6-Touch-AMOLED-2.16`。固件通过 NVS 在四种启动模式间分派：应用选择器、小智语音、BLE HID 键盘和 SD 卡录音器。

核心技术栈为 C/C++、ESP-IDF 5.5.3、FreeRTOS、LVGL、Bluedroid BLE HID、FATFS/SDSPI、ESP-SR 和 NVS。目标芯片是 ESP32-C6，默认分区表为 `partitions/v2/16m_c3.csv`。

## 2. 目录导航

| 目录 | 职责 | 关键说明 | AGENTS.md |
| --- | --- | --- | --- |
| `main/` | 固件入口、语音主循环、音频、显示、协议与板级适配 | `main/main.cc` 负责模式分派 | 本次未递归覆盖 |
| `main/apps/` | 选择器、键盘和录音三个独立应用 | 模式值由 `AppModeRead()` 读取 | 本次未递归覆盖 |
| `main/boards/waveshare/esp32-c6-touch-amoled-2.16/` | 目标板 GPIO、PMIC、AMOLED、触摸与 SD 初始化 | SPI2 共享约束是最高风险点 | [开发指南](main/boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md) |
| `main/sdcard/` | SDSPI 挂载、自检和可选日志落盘 | `SdCardLogStart()` 不能用于小智或录音模式 | 本次未递归覆盖 |
| `scripts/` | 构建、发布、资源和真机稳定性检查 | 串口脚本必须保持安全 DTR/RTS 状态 | [开发指南](scripts/AGENTS.md) |
| `docs/` | 设计、计划与录音防回归说明 | 录音改动前先读 `docs/recorder-design-guardrails.md` | 本次未递归覆盖 |
| `partitions/` | Flash 分区表 | 当前 C6 使用 `v2/16m_c3.csv` | 本次未递归覆盖 |

本次按用户批准采用聚焦文档范围，只覆盖根目录、目标板目录和 `scripts/`；未声称全仓递归覆盖。

## 3. 核心场景索引

- **启动模式分派**
  - 入口：`app_main()`（`main/main.cc`）
  - 核心逻辑：`AppModeRead()`（`main/apps/app_mode.cc`）
  - 副作用：读取 NVS `appsel/mode`，进入 `RunKeyboardApp()`、`RunRecorderApp()`、`RunAppSelector()` 或 `Application::Run()`。
- **从选择器进入应用**
  - 入口：`OnSelectorButtonClicked()`（`main/apps/app_selector.cc`）
  - 核心逻辑：`AppModeWriteAndReboot()` / `AppModeWriteKeyboardAndReboot()`
  - 副作用：提交 NVS 后调用 `esp_restart()`。
- **小智语音启动**
  - 入口：`Application::Initialize()` / `Application::Run()`（`main/application.cc`）
  - 板级初始化：`WaveshareEsp32c6TouchAMOLED2inch16` 构造函数
  - 副作用：启动 AMOLED、触摸、音频、Wi-Fi 和协议任务。
- **BLE HID 键盘**
  - 入口：`RunKeyboardApp()`（`main/apps/ble_keyboard/keyboard_app.cc`）
  - 核心逻辑：`BleHidKeyboard::Init()`、`KeyboardTouchArrows`
  - 副作用：广播 `XiaoZhi KB`，发送键盘报告；配置 2 可切换触区图。
- **SD 卡录音和回放**
  - 入口：`RunRecorderApp()`（`main/apps/recorder/recorder_app.cc`）
  - 核心逻辑：`RecorderControlReduce()`、`RecorderNoiseReducer`、`RecorderRateConverter`
  - 副作用：读写 `/sdcard/rec/recN.wav`，停止后可经串口回传 base64 WAV。
- **AMOLED 与 SD 卡共享 SPI2**
  - 入口：目标板构造函数和 `RecorderDisplayPause()` / `RecorderDisplayResume()`
  - 核心逻辑：SD 初始化或大块 I/O 前停止 LVGL 并取得 LVGL 锁
  - 失败表现：`spi_hal_setup_trans ... running_cmd == 0`，设备约每秒重启并闪屏。
- **真机稳定性验证**
  - 入口：`scripts/verify_selector_stability.py`、`scripts/verify_xiaozhi_stability.py`
  - 核心逻辑：只监听 USB 串口，不主动复位
  - 检查项：选择器 SPI 断言、重复启动、小智 `Stack protection fault` 和 `SdCardLogVprintf` 堆栈。

## 4. 全局设计约束

- 固定使用 ESP-IDF 5.5.3 和 `esp32c6`；干净构建必须保留 `CONFIG_BOARD_TYPE_WAVESHARE_ESP32_C6_TOUCH_AMOLED_2_16=y`。
- GPIO12/GPIO13 是 USB D-/D+，禁止配置为普通 GPIO。GPIO9 是 BOOT，GPIO10 是左键；中间 PWR 键属于 AXP2101，不是普通 GPIO。
- AMOLED 与 SD 卡共用 SPI2（GPIO0/1/2，独立 CS）。禁止在 LVGL 刷新未暂停时初始化 SDSPI 或执行录音模式的大块 SD I/O。
- 小智模式禁止调用 `SdCardLogStart()`：`sys_evt` 栈只有 2304 字节，`SdCardLogVprintf()` 同步执行 `vsnprintf`、FATFS 和 `fwrite` 会触发栈保护故障。录音模式同样禁止启用该 hook，以免日志写卡与刷屏争用 SPI2。
- SD 卡仍可在小智和录音模式挂载；禁用的是同步日志落盘，不是文件读写。小智/录音日志走 USB 串口。
- 不要用不受控的 DTR/RTS 序列做运行态验证。安全监听状态为 `dtr=True, rts=False`；异常时做拔 USB、拔电池、等待 20 秒后的物理冷启动。
- 修改 NVS、OTA 或分区后，至少验证冷启动、软重启和三种应用切换。不要覆盖用户的全盘备份或无理由擦除 NVS。
- 录音改动必须执行 `docs/recorder-design-guardrails.md` 中列出的主机测试，并运行 `idf.py build`。

### 已确认故障（2026-07-11）

| 故障 | 根因 | 修复 | 回归检查 |
| --- | --- | --- | --- |
| 录音模式返回菜单后持续闪屏 | LVGL 首帧刷新与 `SdCardMount(false)` 同时操作 SPI2，触发 `spi_hal_setup_trans` 断言并重启 | 目标板构造函数在挂载 SD 前执行 `lvgl_port_stop()` + `lvgl_port_lock()`，完成后解锁并恢复 | `scripts/verify_selector_stability.py` |
| 小智 AI 不断重复加载 | `sys_evt` 的 2304 字节栈内经日志 hook 同步写 FATFS，Wi-Fi 扫描日志触发 `Stack protection fault` | 小智板级启动不再安装 `SdCardLogStart()`；SD 仍正常挂载 | `scripts/verify_xiaozhi_stability.py` |

## 5. 常见修改入口

- 新增或调整应用模式：`main/apps/app_mode.*`、`main/main.cc`、`main/apps/app_selector.cc`。
- 调整目标板引脚或初始化顺序：目标板目录的 `config.h` 和 `.cc`；先检查 SPI2、I2C0、USB 引脚冲突。
- 修改录音 UI/音频：`main/apps/recorder/`；不得绕过显示暂停和 SD 访问约束。
- 修改键盘触区：`main/apps/ble_keyboard/keyboard_touch_action.*`、`keyboard_touch_arrows.*`。
- 修改 SD 日志：`main/sdcard/sdcard_log.cc`；若要在系统任务中使用，必须先改为独立日志任务和有界队列，不能简单增大 `sys_evt` 栈掩盖同步 I/O。

## 6. AGENTS 维护规则

- 修改已文档化目录中的代码、配置或脚本时，任务结束前必须同步更新该目录的 `AGENTS.md`。
- 若改动使父级导航、场景索引、全局约束或故障记录失效，必须同步更新根 `AGENTS.md`。
- 新增重要目录时应补建就近 `AGENTS.md`；重命名或删除文件后必须清理所有失效引用。
- README 面向使用者，AGENTS 面向维护者；真机限制和高风险禁止事项应在两处保持一致。
- 除非用户明确批准，不得把文档维护留到后续任务。
