# Mic 模块开发指南

> 最后更新：2026-08-08
> 位置：`main/apps/mic/`

## 1. 模块概述

Mic 应用把 ES7210 MIC1/MIC2 作为真实左/右声道，以 24 kHz TDM 同时采集，
重采样为 16 kHz 后使用 ESP-SR NS 降噪，并通过 Wi-Fi UDP 以 20 ms 为周期
实时发送 PCM16 立体声音频。电脑端 `scripts/mic_wifi_receiver.py` 可立即写入
BlackHole 等虚拟声卡；WAV 保存是设备端和电脑端都可选的旁路功能，不参与、
不阻塞实时传输。

ESP32-C6 不支持 Classic Bluetooth HFP，也没有可用的 LE Audio 控制器能力；
自定义 BLE GATT + Opus 的真机结果只有约 0.42x，瓶颈是单核 C6 的双路 NS，
不能作为实时麦克风。因此正式 Mic 链路使用 Wi-Fi，不保留“录完再发送”路径。

## 2. 核心代码结构

| 文件 | 职责 | 关键类 / 方法 |
| --- | --- | --- |
| `mic_app.cc` | PMIC、显示、SD、codec、按键和持续采集主循环 | `StartMicApp()`、`RunMicApp()` |
| `mic_controls.*` | KEY/BOOT/PWR/录音/MENU 到运行时动作的纯映射 | `MicActionForInput()`、`MicGainAfterAction()` |
| `mic_display.*` | Wi-Fi 实时状态、增益、START/STOP 与 MENU | `MicDisplayInit()`、`MicDisplayRender()` |
| `mic_stereo_processor.*` | 双通道重采样、共享 mid NS、side 保留和限幅 | `MicStereoProcessor::Process()` |
| `mic_wifi_audio.*` | 保存 Wi-Fi 连接、接收订阅心跳和 UDP 单播 | `MicWifiAudio::Init()`、`SendPcm()` |
| `mic_wifi_protocol.*` | 与平台无关的控制包和 24 字节音频头 | `MicWifiBuildListenRequest()`、`MicWifiWriteAudioHeader()` |
| `mic_recording_utils.*` | 增益映射与 WAV 头 | `MicInputGainDb()`、`MicBuildStereoWavHeader()` |
| `*_test.cc` | 声道/降噪调度、UDP 协议、WAV 与增益主机测试 | 各自独立 `main()` |

## 3. 核心业务流程

- **启动**：`StartMicApp()` 创建 40 KiB 独立任务 → 初始化 I2C0/AXP2101、
  AMOLED、SD 和 ES7210 → 同时打开 channel 0/MIC1 与 channel 1/MIC2 →
  创建一套 ESP-SR NS → 初始化 `WifiManager`/lwIP 后创建 UDP socket。
- **持续实时传输**：应用启动后始终采集和处理，不需要按 START。每 20 ms
  读取 480 个 24 kHz stereo frame，转换为 320 个 16 kHz stereo frame，
  立即组成一包 UDP PCM 发给当前订阅电脑。
- **电脑订阅**：接收器每秒向 UDP 45830 发送 8 字节 LISTEN 心跳；设备只向
  心跳来源单播，租约 3.5 秒。没有接收器时不广播隐私音频。
- **本地录音**：触摸 START/STOP 只控制 `/sdcard/mic/micNNNN.wav` 的旁路写入；
  每块必须先发送实时 UDP 再写入 32 KiB 缓冲的 WAV，实时采集和 Wi-Fi 发送
  绝不能随 START/STOP 暂停。
- **按键**：KEY/GPIO10 以 10% 步长提高 MIC1/MIC2 共同输入增益，BOOT/GPIO9
  降低增益；AXP2101 PWR 短按只切换屏幕，采集、网络和录音继续。
- **退出**：MENU 先完成可选 WAV，再写 NVS `selector` 并重启。

## 4. C6 实时降噪与立体声约束

- MIC1 必须是 L，MIC2 必须是 R；禁止复制 mono、交换通道或只采一个麦克风。
- `MicStereoProcessor` 先分别重采样 L/R，再计算共同声场 mid。ESP-SR NS 只
  处理 mid，所得共同修正量同时作用于 L/R，因此 `L-R` 的 side 空间信息保持
  不变（安全限幅削顶时除外）。
- C6 正式使用 `low_cpu_realtime_mode=true`：每个 20 ms 包的前 10 ms 完整调用
  `ns_process()`，后 10 ms 使用前帧 ESP-SR 得出的共同衰减增益。真机测得 DSP
  约 12.3–12.7 ms/包，Wi-Fi 活跃时采集和发送均为 1.00x；禁止恢复左右两套
  NS 或每包两次完整 NS，它们分别只有约 0.52x 和 0.89x。
- NS 分配失败时允许保持真实立体声并显示 `NS OFF`；禁止只给单侧降噪。
- `ns_pro_create()`、完整 AFE 和 ESP-SR AGC 在 C6 上有已知内存/稳定性风险，
  未重新完成真机压力测试前不得替换普通 `ns_create(10)`。

## 5. Wi-Fi UDP 协议

- 端口：`45830`；协议版本：`1`。
- LISTEN：`XMIC`、version=1、type=1、reserved=0，共 8 字节。
- AUDIO：24 字节小端头 + 1280 字节 PCM，共 1304 字节，低于常见 MTU：
  `magic[4]`、`version:u8`、`type=2:u8`、`header_size:u16`、
  `sequence:u32`、`timestamp_frames:u32`、`sample_rate=16000:u32`、
  `frames=320:u16`、`channels=2:u8`、`bits=16:u8`。
- 每包正好 20 ms，MIC1/L 在前、MIC2/R 在后交织。协议不兼容修改必须递增
  version，并同步固件、Python 接收器和两端测试。
- Socket 必须在 `WifiManager::Initialize()` 之后创建，否则 lwIP 会触发
  `tcpip_send_msg_wait_sem (Invalid mbox)`。
- 接收使用 `MSG_DONTWAIT`；发送可有界等待最多 3 ms，避免 lwIP 队列瞬时满时
  丢包。不得在采集任务中引入无界网络等待。
- 电脑要成为系统麦克风时，使用 `--play-to` 输出到 BlackHole 等虚拟回环设备；
  接收器先验证 output→input 回送，收到首包后自动切换 macOS 默认输入，断流或
  退出时恢复。用户级 LaunchAgent 可让接收器常驻等待设备进入 Mic，无需每次
  手动启动命令。`--wav` 只是同步备份，不能改成先写完 WAV 再发送。

## 6. 硬件、显示与文件并发

- AMOLED 与 SD 共用 SPI2。`access()`、`mkdir()`、`fopen()`、`fwrite()`、
  头回写、flush/sync/close 都必须在 `RecorderDisplayPause()` / `Resume()` 内。
- 息屏保留一层可嵌套 display pause；触摸不能唤醒，PWR 任务只向主任务发请求。
- GAP/LVGL/Button/Wi-Fi 回调不得访问 SD 或 codec；只修改原子状态或发布请求。
- 无音频时至少让任务阻塞；持续处理时周期性让出 CPU，避免单核 C6 的 IDLE0
  无法喂任务看门狗。
- Mic 模式禁止安装同步 SD 日志 hook。
- 正式运行需要已有 Wi-Fi 凭据；没有凭据时先在小智模式完成配网，Mic 不得把
  SSID/密码硬编码到固件或仓库。

## 7. 验证

主机测试：

```bash
c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/mic \
  main/apps/mic/mic_controls_test.cc \
  main/apps/mic/mic_controls.cc \
  main/apps/mic/mic_recording_utils.cc -o /tmp/mic_controls_test && \
  /tmp/mic_controls_test

c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/mic \
  main/apps/mic/mic_wifi_protocol_test.cc \
  main/apps/mic/mic_wifi_protocol.cc -o /tmp/mic_wifi_protocol_test && \
  /tmp/mic_wifi_protocol_test

c++ -std=c++17 -Wall -Wextra -Werror -I main/apps/mic \
  main/apps/mic/mic_recording_utils_test.cc \
  main/apps/mic/mic_recording_utils.cc -o /tmp/mic_recording_utils_test && \
  /tmp/mic_recording_utils_test

c++ -std=c++17 -Wall -Wextra -Werror \
  -I main/apps/mic -I main/apps/recorder \
  main/apps/mic/mic_stereo_processor_test.cc \
  main/apps/mic/mic_stereo_processor.cc \
  main/apps/recorder/recorder_rate_converter.cc \
  -o /tmp/mic_stereo_processor_test && /tmp/mic_stereo_processor_test

PYTHONPATH=scripts python3 scripts/test_mic_wifi_receiver.py
python3 scripts/test_verify_mic_runtime.py
```

真机验收至少覆盖：四项选择器触区；共享 NS 初始化；设备与接收器连续 30 秒
均接近 1.00x；`missing=0` / `dropped=0`；单侧声学刺激确认真实 L/R；KEY/BOOT
增益；PWR 息屏不断流；START/STOP 不影响实时流且 WAV 可读；MENU 正常返回。

本目录职责、协议或硬件约束变化时必须同步更新本文件以及父级 AGENTS 文档。
