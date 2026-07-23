# XiaoZhi KB 协作开发指南

> 最后更新：2026-07-23

## 1. 系统概述

本仓库基于小智 ESP32 开源固件，主要适配 `Waveshare ESP32-C6-Touch-AMOLED-2.16`。固件通过 NVS 在四种启动模式间分派：应用选择器、小智语音、BLE HID 键盘/陀螺仪空鼠和金山 AI 语音助手（内部模式名仍为 recorder）。

核心技术栈为 C/C++、ESP-IDF 5.5.3、FreeRTOS、LVGL、Apache NimBLE HID、FATFS/SDSPI、ESP-SR 和 NVS。目标芯片是 ESP32-C6，默认分区表为 `partitions/v2/16m_c3.csv`。

## 2. 目录导航

| 目录 | 职责 | 关键说明 | AGENTS.md |
| --- | --- | --- | --- |
| `main/` | 固件入口、语音主循环、音频、显示、协议与板级适配 | `main/main.cc` 负责模式分派 | 本次未递归覆盖 |
| `main/apps/` | 选择器、键盘和金山 AI 三个独立应用 | 模式值由 `AppModeRead()` 读取 | 本次未递归覆盖 |
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
- **BLE HID 键盘与空鼠**
  - 入口：`RunKeyboardApp()`（`main/apps/ble_keyboard/keyboard_app.cc`）
  - 核心逻辑：`BleHidKeyboard::Init()`、`KeyboardTouchArrows`、`AirMouseMotion`
  - 副作用：广播 `XiaoZhi KB`；同一 HID report map 用 Report ID 1 发送九宫格触摸键盘、Report ID 2 发送相对鼠标。所有历史键盘 profile 在运行时统一为配置 2，直接复用 I2C0 上的 QMI8658，BLE 连接期间持续发送空鼠位移；启动时自动点亮九宫格提示屏，BOOT/GPIO10 为鼠标左/右键，PWR 短按 IRQ 或松键沿切换亮屏/暗屏，不存在独立空鼠配置。
- **金山 AI 助手交互、录音和回放**
  - 入口：`RunRecorderApp()`（`main/apps/recorder/recorder_app.cc`）
  - 核心逻辑：`RecorderBuildAssistantUi()`、`RecorderControlReduce()`、`RecorderNoiseReducer`、`RecorderRateConverter`
  - 展示规则：`金山 AI` 静态界面只保留一个主按钮，语义为 `点击说话` / `发送` / `暂停` / `继续`；发送、思考和接收阶段禁用主按钮。
  - 字体规则：固定中文文案从 `xiaozhi-fonts/ttf/puhui-common.ttf` 生成 `font_puhui_assistant_24_4.c` 子集；动态历史文本复用 assets 中的 `font_puhui_common_30_4.bin`，不得把完整字体重复链接进应用镜像。
  - 历史规则：只读取不超过 16 KiB 的正式 `turn.json`，将 `transcript` / `reply_text` 映射到 `你` / `AI 回复`；优先按追加式 `turns.jsonl` 的完成顺序排列旧 turn，每次展示重置到最新记录所在的顶部，截断不得拆开一轮；缺失或损坏时退回文件大小，禁止读取 `.part` / `.bak`。
  - 电源键规则：AXP2101 PWR 短按仅切换 AMOLED 息屏/亮屏；录音、播放、Wi-Fi、WSS、Agent 和 SD 工作继续，触摸不唤醒，长按仍由硬件管理。
  - 副作用：新 turn 写 `/sdcard/agent/YYYYMMDD/<turn>/user.wav`，回复验证后写 `assistant.wav` 和原子清单；旧 `/sdcard/rec/recN.wav` 仅保留播放兼容。
- **Agent 语音助手传输**
  - 入口：`RecorderNetwork`、`AgentVoiceParseControl()`、`AgentTurnStore`
  - 核心逻辑：持久 WSS、ready 帧用户时区时间基准、4096 字节有界传输、turn 幂等状态、服务端 `retryable` 失败分类和 SD 原子文件
  - 副作用：将用户与助手 WAV/清单写入 `/sdcard/agent/`；回复块落卡后才发送累计字节 ACK；确定性失败保留用户 WAV、原子标记 `failed` 并退出待发送队列。
- **AMOLED 与 SD 卡共享 SPI2**
  - 入口：目标板构造函数和 `RecorderDisplayPause()` / `RecorderDisplayResume()`
  - 核心逻辑：SD 初始化或大块 I/O 前停止 LVGL 并取得 LVGL 锁；金山 AI 息屏额外持有一层可嵌套 pause，SH8601 命令只在 Recorder 主任务执行
  - 失败表现：`spi_hal_setup_trans ... running_cmd == 0`，设备约每秒重启并闪屏。
- **真机稳定性验证**
  - 入口：`scripts/verify_selector_stability.py`、`scripts/verify_xiaozhi_stability.py`
  - 核心逻辑：只监听 USB 串口，不主动复位
  - 检查项：选择器 SPI 断言、重复启动、小智 `Stack protection fault` 和 `SdCardLogVprintf` 堆栈。

## 4. 全局设计约束

- 固定使用 ESP-IDF 5.5.3 和 `esp32c6`；干净构建必须保留 `CONFIG_BOARD_TYPE_WAVESHARE_ESP32_C6_TOUCH_AMOLED_2_16=y`。
- GPIO12/GPIO13 是 USB D-/D+，禁止配置为普通 GPIO。GPIO9 的 BOOT 是鼠标左键，GPIO10 是鼠标右键；中间 PWR 键属于 AXP2101，不是普通 GPIO。
- 键鼠配置 2 的 QMI8658（地址 `0x6B`）必须复用键盘触摸初始化的 I2C0（GPIO8 SDA / GPIO7 SCL），不得再创建第二条同引脚 I2C 总线。SensorLib 固定到 commit `baa3e0b83c256b74d9870a95d96d55595946926c`，升级前要重新验证新 I2C API、动态零偏校准和 C6 构建。
- BLE HID 描述符变化后，macOS 可能继续缓存旧的纯键盘描述符；真机验证前先在蓝牙设置中忽略 `XiaoZhi KB` 再重新配对。鼠标移动与按键报告共用短时互斥，禁止在持锁期间延时或执行 IMU/I2C 访问。
- 键鼠模式同样禁止安装 `SdCardLogStart()`：BLE GAP/HID 事件任务不得同步等待 FATFS/SPI，否则会造成 supervision timeout（HCI reason `0x08`）和输入假死。SD 可以保持挂载，日志只走 USB 串口。
- BLE HID 必须使用 ESP-IDF 的 Apache NimBLE 后端，保持通知路径非阻塞；`esp_hid_gap.c` 在连接后请求 15–30 ms interval 和 6 s supervision timeout。空鼠报告固定为 50 Hz，报告发送暂时失败时至少退避 100 ms，禁止切回 Bluedroid 的同步 GATTS 通知路径或恢复为每 tick 重试和错误日志风暴。
- NimBLE HID 广播必须使用 `BLE_HS_FOREVER` 持续到连接建立，不能恢复 ESP-IDF HID 示例的 180 秒演示超时；断开连接后必须重新进入可发现状态。
- `esp_hidd_dev_init()` 注册完 NimBLE HID 的 `ble_hs_cfg` 回调后，必须调用 `ble_store_config_init()`、设置 `ble_hs_cfg.store_status_cb` 并用 `esp_nimble_enable()` 启动 Host task；仅调用 `esp_nimble_init()` 不会运行 Host，因而不会触发 `ESP_HIDD_START_EVENT` 或发出任何广播。
- NimBLE GAP service name、广播名和 HID 配置名必须统一为 `XiaoZhi KB`；不能保留 sdkconfig 默认的 `nimble`，否则 macOS 扫描时显示 `XiaoZhi KB`、连接后却登记成另一台 `nimble` 设备。NimBLE Host 日志保持 Warning，禁止为每个 50 Hz mouse notify 输出 INFO。
- NimBLE HID 必须使用 Security Mode 1 Level 2、Just Works bonding 和 NVS bond 持久化，并在 `BLE_GAP_EVENT_CONNECT` 后调用 `ble_gap_security_initiate()`；Level 0 只会让 macOS 建立临时 GATT 链路，蓝牙设置持续转圈后由主机以 HCI `0x13` 主动断开。
- AMOLED 与 SD 卡共用 SPI2（GPIO0/1/2，独立 CS）。键鼠模式必须先按 AMOLED 最大传输尺寸创建总线，再由 SD 复用；不得让 SD 以 4 KiB 上限先创建总线。所有 SH8601 LVGL 显示路径都必须注册刷新区域偶数起点/奇数终点回调。禁止在 LVGL 刷新未暂停时初始化 SDSPI 或执行录音模式的大块 SD I/O。
- 键鼠 PWR 息屏必须停止 LVGL 并持续持有显示锁；亮屏时先开启 SH8601、在锁内重建并 invalidate 九宫格，再解锁、恢复 LVGL 并唤醒任务。禁止在面板关闭期间继续提交刷新帧。
- 触摸任务读取/清除 PWR IRQ 后必须先释放共享 I2C0 锁，再切换 SH8601/LVGL；只有首次显示上电所需的 PMIC 寄存器事务可以由显示模块短暂取得该锁。
- 金山 AI 模式的 AXP2101 PMIC 轮询任务只能向 Recorder 请求队列发布 PWR 事件，禁止直接调用 SH8601、LVGL、SD、codec 或网络；息屏 pause 必须与现有 SD pause 正确嵌套。
- 小智模式禁止调用 `SdCardLogStart()`：`sys_evt` 栈只有 2304 字节，`SdCardLogVprintf()` 同步执行 `vsnprintf`、FATFS 和 `fwrite` 会触发栈保护故障。录音模式同样禁止启用该 hook，以免日志写卡与刷屏争用 SPI2。
- SD 卡仍可在小智和录音模式挂载；禁用的是同步日志落盘，不是文件读写。小智/录音日志走 USB 串口。
- 不要用不受控的 DTR/RTS 序列做运行态验证。安全监听状态为 `dtr=True, rts=False`；异常时做拔 USB、拔电池、等待 20 秒后的物理冷启动。
- 修改 NVS、OTA 或分区后，至少验证冷启动、软重启和三种应用切换。不要覆盖用户的全盘备份或无理由擦除 NVS。
- 录音改动必须执行 `docs/recorder-design-guardrails.md` 中列出的主机测试，并运行 `idf.py build`。
- ESP-IDF FATFS 的 `rename()` 不能覆盖已存在目标。Agent turn 的 `turn.json` 和
  `assistant.wav` 必须通过 `.part` + 可恢复 `.bak` 交换发布；启动扫描要能从
  `turn.json.bak` 恢复，`.part` / `.bak` 永远不能播放。主机回归必须运行模拟
  `FR_EXIST` 的 `agent_turn_store_fatfs_test.cc`，不能只依赖 macOS 的 POSIX rename。
- Agent 回复禁止在网络回调中直接写 SD；主任务每成功落卡一个块后发送 `reply_chunk_saved`，服务端收到精确累计字节 ACK 才能继续发送。`assistant.wav.part` 永远不可播放。
- 设备必须区分服务端终态失败与暂时失败：`retryable=false` 的当前 turn 要在主任务中原子标记 `failed`、清除 queued 状态并恢复录音；`ListPending()` 永远排除 `failed`，但不得删除其 `user.wav`。只有 `retryable=true` 才允许断线重放。
- turn 日期和 ID 只能由纯 `RecorderTurnClock` 生成：认证 ready 帧的 Unix 毫秒与用户 UTC 偏移配合单调时钟得到本地 `YYYYMMDD`；未同步时使用八字符 `unsynced`。禁止恢复 `%llu` 格式化或把无效墙钟写成 `19700101`，ESP-IDF nano printf 不保证 64 位格式支持。
- 单个上传 WAV 上限为 4 MiB；录音链路必须预留 DSP flush 空间并自动停止，不能生成永久无法上传的队列项。设备 token 只允许存在于忽略的 `sdkconfig` 和构建产物。
- 助手 UI 必须通过纯 `RecorderAssistantUiInput` → `RecorderAssistantUiModel` 映射渲染，不在显示回调中访问 SD、网络或 codec；保持静态组件，不增加 LVGL 动画或 UI 定时器。
- 动态对话字库只允许通过 `Assets` 内存映射现有 `font_puhui_common_30_4.bin`；加载失败必须非致命回退。对话详情必须自动换行并使用内容高度，禁止恢复固定高度加 `LV_LABEL_LONG_DOT` 的裁字布局；历史清单读取仍由暂停 LVGL 后的 Recorder 主任务发起。

### 已确认故障（2026-07-11）

| 故障 | 根因 | 修复 | 回归检查 |
| --- | --- | --- | --- |
| 录音模式返回菜单后持续闪屏 | LVGL 首帧刷新与 `SdCardMount(false)` 同时操作 SPI2，触发 `spi_hal_setup_trans` 断言并重启 | 目标板构造函数在挂载 SD 前执行 `lvgl_port_stop()` + `lvgl_port_lock()`，完成后解锁并恢复 | `scripts/verify_selector_stability.py` |
| 小智 AI 不断重复加载 | `sys_evt` 的 2304 字节栈内经日志 hook 同步写 FATFS，Wi-Fi 扫描日志触发 `Stack protection fault` | 小智板级启动不再安装 `SdCardLogStart()`；SD 仍正常挂载 | `scripts/verify_xiaozhi_stability.py` |

## 5. 常见修改入口

- 新增或调整应用模式：`main/apps/app_mode.*`、`main/main.cc`、`main/apps/app_selector.cc`。
- 调整目标板引脚或初始化顺序：目标板目录的 `config.h` 和 `.cc`；先检查 SPI2、I2C0、USB 引脚冲突。
- 修改金山 AI UI/音频：`main/apps/recorder/`；不得绕过显示暂停和 SD 访问约束。新增固定中文前更新字体子集和展示模型测试；修改动态历史文本时同时运行 `recorder_playback_menu_test.cc`、`recorder_history_layout_test.cc` 与 `recorder_common_font_test.cc`。
- 修改键盘触区或空鼠：`main/apps/ble_keyboard/keyboard_touch_action.*`、`keyboard_touch_arrows.*`、`air_mouse_motion.*`、`one_euro_filter.*`。空鼠正常移动直接映射陀螺仪角速度，不引入 Madgwick 绝对姿态；板级轴向/符号集中在 `AirMouseTask()` 两行映射处，真机调整时同时检查静止漂移、慢速抖动、快速延迟，以及 BLE 断开后立即停止并清空残余位移。
- 修改 SD 日志：`main/sdcard/sdcard_log.cc`；若要在系统任务中使用，必须先改为独立日志任务和有界队列，不能简单增大 `sys_evt` 栈掩盖同步 I/O。

## 6. AGENTS 维护规则

- 修改已文档化目录中的代码、配置或脚本时，任务结束前必须同步更新该目录的 `AGENTS.md`。
- 若改动使父级导航、场景索引、全局约束或故障记录失效，必须同步更新根 `AGENTS.md`。
- 新增重要目录时应补建就近 `AGENTS.md`；重命名或删除文件后必须清理所有失效引用。
- README 面向使用者，AGENTS 面向维护者；真机限制和高风险禁止事项应在两处保持一致。
- 除非用户明确批准，不得把文档维护留到后续任务。
