# scripts 工具开发指南

> 最后更新：2026-07-11
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
| `verify_ble_keyboard_after_cold_boot.py` | 冷启动后监听键盘日志并扫描 `XiaoZhi KB` | 可选依赖 `bleak`；不主动复位 |
| `verify_selector_stability.py` | 检测选择器 SPI2 断言与重启循环 | 查找 `spi_hal_setup_trans` 和重复 selector 启动 |
| `verify_xiaozhi_stability.py` | 检测小智 Wi-Fi 启动栈故障 | 查找 `Stack protection fault`、重复启动和 `SdCardLogVprintf` |
| `verify_agent_voice_runtime.py` | 验收 Agent 完整语音回路 | 安全监听至少 30 秒，要求联网、WSS、双 WAV 落卡和自动播放里程碑；SPI2/栈/哈希/重启故障硬失败 |
| `test_verify_agent_voice_runtime.py` | 串口判定规则单元测试 | 纯日志样本，不连接真机、不读取秘密 |

子目录 `Image_Converter/`、`ogg_converter/`、`p3_tools/`、`spiffs_assets/` 和 `acoustic_check/` 各自提供资源转换或声学工具；优先阅读其中 README。

## 3. 设计约定

- 真机监听必须使用 `serial.Serial(port=None)`，先设 `dtr=True`、`rts=False` 再 `open()`。禁止为了“方便”切回自动 DTR/RTS 复位。
- 稳定性脚本只观察、不修改设备状态；无串口端口返回 2，发现目标故障返回 1，稳定返回 0。
- 检测规则应锚定真实 panic 文本和启动日志，避免只靠屏幕现象判断。
- 新增 Python 脚本至少执行 `python -m py_compile`；涉及固件产物的脚本还需运行对应构建命令。
- 不在仓库中写入 token、Cookie、Wi-Fi 密码或设备私密日志。

## 4. 典型调用

```bash
/tmp/ser-venv/bin/python scripts/verify_selector_stability.py --duration 6
/tmp/ser-venv/bin/python scripts/verify_xiaozhi_stability.py --duration 15
/tmp/ser-venv/bin/python scripts/verify_agent_voice_runtime.py --duration 30
/tmp/ser-venv/bin/python scripts/verify_ble_keyboard_after_cold_boot.py
```

稳定性测试应覆盖目标故障出现所需的时间窗口：选择器至少 6 秒，小智至少 15 秒以越过 Wi-Fi 扫描阶段。

## 5. 维护风险

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| 串口脚本意外切换下载模式 | 中 | 高：设备黑屏或看似失联 | 保持安全 DTR/RTS；必要时物理冷启动 |
| 检测窗口太短导致假通过 | 中 | 高：重启缺陷漏检 | selector 6 秒、小智 15 秒作为最低值 |
| 发布脚本使用错误板型/分区 | 低 | 高：固件不可启动 | 发布前核对 `sdkconfig.defaults.esp32c6` 和 `16m_c3.csv` |
