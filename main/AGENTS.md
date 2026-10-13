# main 固件组件开发指南

> 最后更新：2026-10-13
> 位置：`main/`

## 1. 概述

`main` 是 ESP-IDF 主组件：`app_main()` 完成 NVS 初始化和启动模式分派，
`Application` 承担小智语音模式的事件循环，`apps/` 提供选择器、键鼠、神勇
与 Mic 独立应用。神勇整段回复下载依赖 `esp_http_client`，单次 GET 有界写卡后自动播放。`CMakeLists.txt` 同时决定板级实现、字体、音频编码器和所有应用源文件是否进入最终镜像。

## 2. 核心代码结构

| 文件/目录 | 职责 | 关键类 / 方法 |
| --- | --- | --- |
| `main.cc` | 固件入口与模式分派 | `app_main()`、`AppModeRead()`、`StartRecorderApp()`、`StartMicApp()` |
| `application.*` | 小智模式的主事件循环、音频与协议协调 | `Application::Initialize()`、`Application::Run()`、`Application::Schedule()` |
| `device_state_machine.*` | 校验并广播设备状态迁移 | `DeviceStateMachine::TransitionTo()`、`CanTransitionTo()` |
| `assets.*` | assets 分区映射、字体/模型资源应用和下载 | `Assets::Apply()`、`GetAssetData()`、`Download()` |
| `mcp_server.*` | MCP 工具注册、参数校验与调用结果封装 | `McpServer::AddCommonTools()`、`ParseMessage()`、`DoToolCall()` |
| `ota.*` | 版本检查、激活和 OTA 写入 | `Ota::CheckVersion()`、`Activate()`、`Upgrade()` |
| `settings.*` | NVS 命名空间读写封装 | `Settings::GetString()`、`SetInt()`、析构时 `nvs_commit()` |
| `apps/` | 五种启动状态中的选择器与四个独立应用 | [应用层指南](apps/AGENTS.md) |
| `audio/codecs/` | 板级 codec 与 I2S 适配 | [Codec 指南](audio/codecs/AGENTS.md) |
| `boards/` | GPIO、PMIC、显示、触摸、网络与具体板型 | 当前目标板见 `boards/waveshare/esp32-c6-touch-amoled-2.16/AGENTS.md` |

## 3. 核心流程

- **启动分派**：`app_main()` 先记录 `esp_reset_reason()`，再经 `AppModeRead()` → `RunKeyboardApp()` / `StartRecorderApp()` / `StartMicApp()` / `RunAppSelector()` / `Application::Run()`；神勇创建 24 KiB 录音任务（AMR 编码搜索超过原 8 KiB main 栈），Mic 创建 40 KiB 独立任务后 `app_main` 返回，以容纳 ESP-SR 与实时 UDP 工作帧。
- **小智语音**：`Application::Initialize()` → `Board::StartNetwork()` → `InitializeProtocol()` → `Application::Run()` 消费事件位并驱动 `AudioService`。
- **模式切换**：选择器触摸回调 → `AppModeWriteAndReboot()` → NVS `appsel/mode` 提交 → `esp_restart()` → 新模式入口。
- **Mic 音频**：`RunMicApp()` → `BoxAudioCodec` 双通道采集 → `MicStereoProcessor` → Wi-Fi UDP 实时 PCM 与可选 SD WAV 两个并行消费者；START/STOP 只影响后者。
- **资源加载**：`Assets::InitializePartition()` → `LvglStrategy` 或 `EmoteStrategy` → 字体、表情与 SR 模型应用。

## 4. 设计约定

- 新应用必须同时更新 `apps/app_mode.*`、`main.cc`、`apps/app_selector.cc`、`main/CMakeLists.txt` 和目标芯片默认配置。
- 独立应用入口运行后不返回；跨任务回调只发布事件，由应用主任务执行 SD、codec、LVGL 和重启操作。
- 当前目标是 ESP32-C6 / ESP-IDF 5.5.3。Mic 的正式实时链路依赖 lwIP/Wi-Fi、
  ESP-SR `ns_create(10)` 和 ES7210 双通道采集，不依赖 Opus 或 NimBLE；仓库中的
  Opus/NimBLE 配置仍分别供小智语音与 BLE 键鼠应用使用。
- BLE 触控板不得在通知暂时失败时丢弃位移；`mouse_move_accumulator.cc` 负责
  有界合并并按 HID 范围排空，必须加入 `main/CMakeLists.txt`，纯逻辑回归见
  `mouse_move_accumulator_test.cc`。
- `Settings` 只有以 `read_write=true` 打开时才允许写入；提交发生在析构阶段，模式切换代码则直接使用 NVS 并在重启前显式提交。
- `main/CMakeLists.txt` 面向多个板型；修改公共 codec 构造器时必须保持默认参数兼容已有板级调用。
- 不得在小智、Recorder 或 Mic 模式安装同步 SD 日志 hook；这些模式的串口日志不能与共享 SPI2 的显示/SD 数据路径竞争。

## 5. 常见修改入口

- 新增启动应用：从 `apps/app_mode.h`、`app_main()` 和 `BuildSelectorUI()` 开始，再登记源文件与 include 路径。
- 修改小智状态流：先检查 `DeviceStateMachine::IsValidTransition()`，再改 `Application::Handle*Event()`。
- 新增 MCP 工具：使用 `McpServer::AddTool()` / `AddUserOnlyTool()`，明确参数范围和执行线程副作用。
- 修改 assets：检查 `Assets::LvglStrategy::InitializePartition()` 的校验与 mmap 生命周期，并同步构建脚本。
- 修改 codec：阅读 `audio/codecs/AGENTS.md`；特别核对采样率、物理通道掩码和 I2S slot 配置。

## 6. 维护与风险

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| 模式枚举已增加但未注册源文件/分派 | 中 | 高：按钮重启后进入错误模式或链接失败 | 同步检查五个入口文件并执行 `idf.py build` |
| 公共组件改动影响所有板型 | 中 | 高：非当前板 codec 或显示回归 | 保持默认参数兼容，运行当前板完整构建 |
| 回调线程直接执行阻塞 I/O | 中 | 高：BLE 超时、栈故障或 SPI2 断言 | 回调只入队，主任务串行处理副作用 |
| 镜像超过最小 app 分区 | 低 | 高：构建或 OTA 失败 | 每次构建检查 `check_sizes.py` 剩余空间 |

修改本目录代码、配置或子模块职责后，必须同步更新本文件；若启动流程或全局约束变化，还需更新仓库根 `AGENTS.md`。

CMake runs the tracked WebSocket lifetime patch before building the managed dependency.
Failed upgrades still own a reader task; always disconnect/join it before freeing the
WebSocket buffers or event group, otherwise a service restart can trigger double-free.
