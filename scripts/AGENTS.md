# scripts 工具开发指南

> 最后更新：2026-10-13
> 位置：`scripts/`

## 1. 概述

本目录包含资源生成、版本发布、音频调试和真机稳定性验证工具。脚本不是固件运行时依赖，但会直接影响构建产物、GitHub Release 和设备诊断结论。

## 2. 核心工具

| 文件 | 用途 | 关键入口/约束 |
| --- | --- | --- |
| `build_default_assets.py` | 根据 `sdkconfig` 生成默认 assets 镜像 | `main()`；输出会被 CMake 烧入 assets 分区 |
| `release.py` | 构建发布包并组织版本产物 | 发布前核对板型和分区 |
| `versions.py` | 解析和维护版本信息 | 被发布流程调用 |
| `download_github_runs.py` | 下载 GitHub Actions 运行产物 | 需要有效 GitHub 访问环境 |
| `gen_lang.py` | 生成语言资源 | 修改后需要重建 assets |
| `audio_debug_server.py` | UDP 8000 接收 PCM 并写 WAV | 参数为采样率和声道数 |
| `mic_wifi_receiver.py` | 订阅 Mic 的 20 ms Wi-Fi UDP PCM 流并实时输出虚拟声卡，可选同步 WAV | `parse_audio_packet()`、`SequenceTracker`；`--play-to` 会验证真实回环并在 macOS 自动切换默认输入 |
| `macos_audio_input.py` | 通过 CoreAudio 查询/切换 macOS 默认输入并安全恢复 | `MacOSAudioInput`、`TemporaryDefaultInput`；不依赖外部音频切换 CLI |
| `install_mic_macos_service.py` | 将接收器、隔离 venv 和 LaunchAgent 安装到当前 macOS 用户 | 安装前验证 BlackHole 双声道回环；进入/退出 Mic 自动切换/恢复输入 |
| `test_mic_wifi_receiver.py` | Mic UDP 格式、sequence wrap/gap 单元测试 | 不连接真机，不依赖音频库 |
| `test_macos_audio_input.py` | CoreAudio fourcc 和设备名匹配纯逻辑测试 | 不修改系统默认输入 |
| `test_install_mic_macos_service.py` | LaunchAgent 路径和参数纯逻辑测试 | 不写 `~/Library`、不调用 `launchctl` |
| `verify_ble_keyboard_after_cold_boot.py` | 冷启动后监听键盘日志并扫描 `XiaoZhi KB` | 可选依赖 `bleak`；不主动复位 |
| `verify_selector_stability.py` | 检测选择器 SPI2 断言与重启循环 | 查找 `spi_hal_setup_trans` 和重复 selector 启动 |
| `verify_xiaozhi_stability.py` | 检测小智 Wi-Fi 启动栈故障 | 查找 `Stack protection fault`、重复启动和 `SdCardLogVprintf` |
| `verify_agent_voice_runtime.py` | 验收 Agent 完整语音回路 | 安全监听至少 30 秒，要求联网、WSS、`/sdcard/shenyong/` 上下行 AMR 落卡和自动播放里程碑；SPI2/栈/哈希/重启故障硬失败 |
| `test_verify_agent_voice_runtime.py` | 串口判定规则单元测试 | 纯日志样本，不连接真机、不读取秘密 |
| `verify_mic_runtime.py` | 验证 Mic 40 KiB 任务、双通道 codec、共享 ESP-SR NS、Wi-Fi UDP 与 1.00x 采集 | 安全 DTR/RTS，只监听不复位；panic/SPI2/任务看门狗硬失败 |
| `test_verify_mic_runtime.py` | Mic 启动日志规则单元测试 | 纯日志样本，不连接真机 |

子目录 `Image_Converter/`、`ogg_converter/`、`p3_tools/`、`spiffs_assets/` 和 `acoustic_check/` 各自提供资源转换或声学工具；优先阅读其中 README。

## 3. 设计约定

- 真机监听必须使用 `serial.Serial(port=None)`，先设 `dtr=True`、`rts=False` 再 `open()`。禁止为了“方便”切回自动 DTR/RTS 复位。
- 稳定性脚本只观察、不修改设备状态；无串口端口返回 2，发现目标故障返回 1，稳定返回 0。
- 检测规则应锚定真实 panic 文本和启动日志，避免只靠屏幕现象判断。
- 新增 Python 脚本至少执行 `python -m py_compile`；涉及固件产物的脚本还需运行对应构建命令。
- Mic 接收器的 UDP 端口、LISTEN 心跳和 24 字节音频头必须与 `main/apps/mic/mic_wifi_audio.cc` / `mic_wifi_protocol.*` 同步；协议不兼容时递增 version，不能静默猜测。
- `--play-to` 把实时 PCM 写入 PortAudio output；macOS 默认用同名 CoreAudio
  input 并在首个实时包后切换系统默认麦克风，停止推流或退出时恢复。目标必须
  通过双声道 output→input 探测，禁止因设备名带“录音”就跳过验证；本机
  `ESHOW录音` 已实测回读全零。`--wav` 只能是同步旁路备份，禁止改为先录后播。
- 常驻接收使用用户级 `com.xiaozhi.wifi-mic` LaunchAgent；安装器必须先验证
  BlackHole 双声道回环，再原子写 plist 并 bootstrap。设备未推流时不得切换输入；
  超过 2 秒断流或进程退出必须恢复原输入。安装系统驱动和启用 LaunchAgent 前
  需要用户明确授权。
- CoreAudio 默认输入切换不会强制迁移应用已经打开的音频会话。端到端验收除检查
  系统默认设备和 BlackHole 非零样本外，还要完全退出并重开切换前已启动的目标应用；
  豆包 2.21.10 没有独立输入选择器，其“系统麦克风”表示当前系统默认设备。
- 不在仓库中写入 token、Cookie、Wi-Fi 密码或设备私密日志。

## 4. 典型调用

```bash
/tmp/ser-venv/bin/python scripts/verify_selector_stability.py --duration 6
/tmp/ser-venv/bin/python scripts/verify_xiaozhi_stability.py --duration 15
/tmp/ser-venv/bin/python scripts/verify_agent_voice_runtime.py --duration 30
/tmp/ser-venv/bin/python scripts/verify_mic_runtime.py --duration 20
/tmp/ser-venv/bin/python scripts/verify_ble_keyboard_after_cold_boot.py
python3 scripts/mic_wifi_receiver.py --play-to BlackHole
PYTHONPATH=scripts python3 scripts/test_mic_wifi_receiver.py
PYTHONPATH=scripts python3 scripts/test_macos_audio_input.py
PYTHONPATH=scripts python3 scripts/test_install_mic_macos_service.py
```

稳定性测试应覆盖目标故障出现所需的时间窗口：选择器至少 6 秒，小智至少 15 秒以越过 Wi-Fi 扫描阶段。

## 5. 维护风险

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| 串口脚本意外切换下载模式 | 中 | 高：设备黑屏或看似失联 | 保持安全 DTR/RTS；必要时物理冷启动 |
| 检测窗口太短导致假通过 | 中 | 高：重启缺陷漏检 | selector 6 秒、小智 15 秒作为最低值 |
| 发布脚本使用错误板型/分区 | 低 | 高：固件不可启动 | 发布前核对 `sdkconfig.defaults.esp32c6` 和 `16m_c3.csv` |
| Mic 两端协议字段不一致 | 中 | 高：无法解码或声道错误 | 同步 UUID/format/header 并运行接收器单测 |
| UDP 发送或接收排队导致缺口 | 中 | 中：虚拟麦克风点击/静音 | 设备发送有界等待 3 ms；接收器按 sequence 统计并对短缺口补静音 |

`patch_recorder_websocket.py` is invoked during CMake configuration for the pinned
esp-ml307 dependency. It makes WebSocket destruction always stop/join the TCP reader,
even after an HTTP upgrade failure, before destroying callback-owned members. The
patch is idempotent and rejects changed upstream code; its test covers both guards.
Never replace it with a delay or disable heap checks.
