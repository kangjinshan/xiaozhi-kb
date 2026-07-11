# ESP32-C6 Recorder ESP-SR Noise Suppression Design

## Goal

让 SD 卡录音模式真正使用 Espressif 官方 ESP-SR 降噪能力，替换当前仅做逐采样噪声门、固定增益和限幅的兜底处理，同时不改变小智语音模式。

## Confirmed Facts

- 目标硬件是无 PSRAM 的 ESP32-C6，麦克风 codec 以 24 kHz 单声道输出。
- 当前 C6 固件没有启用 `AfeAudioProcessor`。`CONFIG_USE_AUDIO_PROCESSOR` 只允许 ESP32-S3/P4 且要求 PSRAM，构建产物实际包含 `NoAudioProcessor`。
- ESP-SR 自检代码也把完整 AFE 支持范围限定为 ESP32/ESP32-S3/ESP32-P4。因此不能通过解除 Kconfig 限制来安全复用完整 AFE。
- 上一版录音器调用 `ns_pro_create(10, 2, 16000)` 后在真机崩溃。该入口需要一次申请约 33 KiB 连续内部 SRAM，分配失败后库仍继续调用 `WebRtcNs_set_policy`，导致空句柄访问。
- ESP-SR 同时提供低内存 `ns_create(10)` / `ns_process()` 接口。它使用多个较小的内存块，原生处理 16 kHz、10 ms、160 个采样点的音频帧，适合在 C6 上单独验证和使用。
- 当前所谓 fallback 只把绝对值不超过 45 的采样清零，再把其余采样放大 6 倍。它不能抑制高于阈值的环境噪声，必须删除。

## Chosen Approach

在录音模式内增加一条独立但复用小智音频基础设施的处理链：

1. `BoxAudioCodec` 继续以 24 kHz 读取麦克风，避免改变整机 I2S 时钟和已有播放行为。
2. 使用小智 `AudioService` 已在使用的 `esp_ae_rate_cvt`，把 24 kHz 单声道 PCM 流式转换为 16 kHz。
3. 按 160 个采样点分帧，调用 ESP-SR 官方低内存 `ns_create(10)` / `ns_process()`。
4. 只在降噪后使用固定 6 倍增益和峰值 30000 的限幅，不启用 AGC，避免 AGC 再次放大静音段残余噪声。
5. 新录音保存为 16-bit、单声道、16 kHz WAV。16 kHz 是该降噪器的原生语音采样率，不再把处理结果无意义地上采样后写盘。
6. 播放器接受现有 24 kHz WAV 和新 16 kHz WAV；播放 16 kHz 文件时使用 `esp_ae_rate_cvt` 转换到 codec 的 24 kHz 输出率。

完整 AFE 和服务端处理均不采用：前者不支持当前 C6/内存配置，后者不会返回可保存的增强 PCM，并且会让离线录音依赖网络。

## Components and Interfaces

### `RecorderNoiseReducer`

- 初始化 24 kHz → 16 kHz 流式重采样器和 `ns_create(10)` 句柄。
- 接受任意长度的 24 kHz PCM 块，把重采样后的数据累积成 160-sample 帧。
- 每个完整帧经 `ns_process()` 后追加到调用方提供的输出缓冲区。
- `Flush()` 向没有显式 flush API 的重采样器送入有限零样本，按累计输入采样数 × 16000 / 24000 计算并裁剪有效输出；不足 160-sample 的 NS 尾帧补零处理后也只保留有效部分。最终输出采样数必须等于累计输入时长按 16 kHz 换算后的取整结果，不能静默丢样或多写补零。
- 提供 `output_sample_rate()`、`noise_reduction_enabled()` 和初始化状态，供 WAV 头、日志和错误处理使用。

为便于主机测试，帧缓冲、尾部处理、增益和限幅与 ESP-IDF 句柄封装分离；主机测试使用可注入的假 NS 后端验证数据流，不能再以“常量 20 会被噪声门清零”冒充降噪测试。

### Recorder app

- WAV 头和录音时长按 reducer 的 16 kHz 输出计算。
- 仅写入 reducer 实际产生的输出，不再原地修改 24 kHz 输入缓冲。
- 停止录音或退出应用时先 `Flush()`，再回填 WAV 头。
- 启动日志打印 NS 初始化前后的空闲内部堆、最大连续块、处理采样率和句柄状态，便于真机确认实际走了官方 NS。

### WAV playback

- 放宽“WAV 采样率必须与 codec 完全相同”的限制，但仍要求 PCM、16-bit、单声道以及受支持的采样率。
- 24 kHz 文件直接播放；16 kHz 文件流式重采样为 24 kHz 后播放。
- 文件结束时排空重采样尾部，避免末尾被截断。

## Failure Handling

- `ns_create(10)` 返回空句柄时绝不调用 `ns_process()`，并在屏幕和串口明确报告 NS 不可用。
- 为保留基本录音功能，初始化失败时仍将 24 kHz 输入转换为 16 kHz 后保存，但不再应用误导性的噪声门；日志必须标记该文件未降噪。
- 重采样器创建或处理失败时停止当前录音并保存可恢复的 WAV 头，不能写入采样率与内容不一致的文件。
- 不重新启用 `ns_pro_create()`，除非未来库版本修复分配失败处理且通过真机连续内存验证。

## Testing and Acceptance

### Automated verification

- 主机单测覆盖跨调用的 160-sample 分帧、任意输入块大小、停止时零填充与裁剪、24 kHz→16 kHz 输出采样数、6 倍增益、30000 限幅和后端失败。
- WAV 单测覆盖 16 kHz/24 kHz 文件识别，以及不支持格式拒绝。
- ESP32-C6 `idf.py build` 必须成功，构建产物必须包含 `ns_create`，且不能包含 recorder 对 `ns_pro_create` 的引用。

### Real-device verification

1. 烧录后进入 Recorder，空闲至少 15 秒；不得出现 `Guru Meditation`、重启循环或 SPI2 并发断言。
2. 串口日志必须明确显示 `ESP-SR NS enabled`，并记录初始化前后堆信息；fallback 日志不算通过。
3. 在固定环境中录制“至少 2 秒环境声 → 正常说话 → 至少 2 秒环境声”，保存并播放成功。
4. 与上一版同环境录音比较：静音段环境底噪应明显下降，语音保持可懂且无明显抽吸、削波或断字。
5. 从 SD 菜单播放一份旧 24 kHz WAV 和一份新 16 kHz WAV，两者都必须正常完成且不重启。

只有自动化检查、真机稳定性、日志确认官方 NS 实际启用，以及实际录音听感四项都通过，才能判定任务完成。
