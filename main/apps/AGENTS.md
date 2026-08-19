# apps 应用模式开发指南

> 最后更新：2026-08-19
> 位置：`main/apps/`

## 1. 模块概述

本目录把同一块设备划分为互斥的启动应用。选择器将模式写入 NVS 后重启，
`app_main()` 在下一次启动进入小智、BLE 键鼠、金山 AI 或实时立体声 Wi-Fi Mic。
各应用自行初始化所需的显示、I2C、codec、网络或 BLE 资源，不能在运行时并存。

## 2. 核心代码结构

| 文件/目录 | 职责 | 关键类 / 方法 |
| --- | --- | --- |
| `app_mode.h` | 定义持久化模式枚举 | `AppMode::kSelector`、`kXiaozhi`、`kKeyboard`、`kRecorder`、`kMic` |
| `app_mode.cc` | NVS 模式读写与重启 | `AppModeRead()`、`AppModeWriteAndReboot()`、`AppModeWriteKeyboardAndReboot()` |
| `app_mode_keys.*` | 键盘 profile NVS key | `KeyboardProfileNvsKey()` |
| `app_selector.cc` | 四个应用按钮的 LVGL 选择界面 | `BuildSelectorUI()`、`OnSelectorButtonClicked()`、`CreateModeButton()` |
| `ble_keyboard/` | BLE HID 键盘和触控板应用 | 见 `ble_keyboard/AGENTS.md` |
| `recorder/` | 金山 AI 录音、传输、回复和历史应用 | 先读 `docs/recorder-design-guardrails.md` |
| `mic/` | 双 ES7210 麦克风、ESP-SR 降噪、Wi-Fi UDP 实时 PCM 和可选 SD 录音 | 见 [Mic 指南](mic/AGENTS.md) |

## 3. 核心业务流程

- **选择应用**：`RunAppSelector()` → `BuildSelectorUI()` → 点击 `SelectorButtonContext` → `AppModeWriteAndReboot()`。
- **启动触摸保护**：四个按钮只接受 UI 完成一秒后新产生的 `PRESSED` → `CLICKED`；启动时 CST9217 遗留触点即使稍后释放也不能自动进入底部 Mic。
- **读取模式**：`app_main()` → `AppModeRead()` 读取 `appsel/mode`；缺失或非法值一律回到 `kSelector`。
- **键鼠 profile**：键鼠按钮调用 `AppModeWriteKeyboardAndReboot()`；历史 profile 在 `KeyboardProfileRead()` 中统一迁移为 profile 2。触控板报告暂时拥塞时由 BLE 子模块有界累计位移并在恢复后分片排空，模式切换时清除残留。
- **Mic 启动**：Mic 按钮写入字符串 `mic` → 重启 → `StartMicApp()` 创建独立任务 → `RunMicApp()` 立即持续发送实时流，应用内 MENU 再写回 `selector`。

## 4. 关键资源与副作用

- NVS namespace：`appsel`。
- NVS mode key：`mode`；有效字符串为 `selector`、`xiaozhi`、`keyboard`、`recorder`、`mic`。
- 键盘 profile key：`kb_profile`。
- 所有成功的模式选择都会 `nvs_commit()`，延时 100 ms 后调用 `esp_restart()`。
- 选择器依赖目标板 `Board::GetDisplay()` 和 LVGL；按钮区域不可依靠滚动才能访问。

## 5. 常见修改场景与切入点

- 新增应用：添加 `AppMode` 枚举与字符串双向映射，增加 `app_main()` 分支和选择器按钮，再把源文件加入 `main/CMakeLists.txt`。
- 调整选择器布局：修改 `BuildSelectorUI()` 和 `CreateModeButton()`，在 480×502 屏上验证所有按钮可见、可点且不滚动。
- 修改键盘 profile：同时检查 `app_mode_keys_test.cc` 和 `ble_keyboard/` 的兼容规则。
- 修改返回菜单行为：独立应用必须先关闭或落盘当前工作，再调用 `AppModeWriteAndReboot(AppMode::kSelector)`。

## 6. 维护与风险说明

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| 枚举、NVS 字符串和入口分支不一致 | 中 | 高：重启后落入 selector 或错误应用 | 对照三处映射并完整构建 |
| 四个按钮超出固定屏幕 | 中 | 中：Mic 无法进入 | 真机检查全部触区，不启用隐藏滚动 |
| 应用退出时未完成资源落盘 | 低 | 高：WAV/清单损坏 | 在主任务处理退出，先 finalize 再重启 |

本目录或任一子应用结构变化后必须更新本文件；新增重要子目录还应补建就近 `AGENTS.md`。
