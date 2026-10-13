# 神勇设备语音模块

本目录是 ESP32-C6 的独立神勇助手应用，持久模式键仍为 `recorder`。修改前阅读
`docs/recorder-design-guardrails.md`，保留 SPI2、SD 原子发布和设备认证。新版一次 HTTPS GET 下载整段 WAV；旧 WSS 客户端的逐块 ACK 仅作兼容。

- `recorder_display.cc`：静态神勇界面，仅有状态与大按钮，不提供历史；主按钮按下开始、释放或移出结束。回调只发布事件。
- `recorder_hold_input.h`：锁存触摸的按住/释放；暗屏触摸有 300 ms 门槛和残留触点抑制。暗屏只在主任务轮询 I2C，LVGL 保持暂停，录音不唤醒屏幕。
- `recorder_control_state.*`：录音、播放、音量与电源的纯状态机；后台发送/等待不能禁止录音。BOOT 减音量、KEY 加音量，在所有状态有效，只改变音量。
- `recorder_recording_policy.h`：以原始采集帧数判断至少 1 秒，不计 AMR 尾帧补齐；短录音在松手和退出时删除当次未索引音频，不能写 manifest、入队或触发网络。
- `recorder_assistant_ui.*`：纯展示映射；录音状态优先于前一条请求的错误提示，始终显示松开发送。
- `recorder_app.cc`：唯一 SD/codec 主循环；每轮最多一个网络事件、一个 4096 字节上传块、一个 1024 样本播放块。上传缓冲区在堆上复用，`StartRecorderApp()` 创建 24 KiB 栈的专用任务，AMR 搜索/TLS/JSON 工作必须保留栈余量。
- `recorder_turn_queue.h`：在途队首固定，保存新的录音只追加，完成/终态失败后才出队。启动后不得再调用会清理 `.part` 的 `ListPending()`。
- `recorder_reply_download.*`：使用严格同源 HTTPS 和原设备 Token，后台 12 KiB 任务完成一次 GET，8 KiB 有界接收缓冲由主任务读取到 SD，不发送逐块 ACK。拒绝重定向/错误状态/长度不符，15 秒无进度重连重取已完成 turn，不能调用模型重新生成。
- `recorder_playback_stream.*`：可暂停的 8/16/24 kHz PCM16 单声道 WAV 与 AMR-NB 游标；AMR 每帧解码为 160 个 8 kHz 样本；与 resampler 一起跨录音保留，不读文件即暂停。录音时禁用输出，松手保存后自动续播，回复按完成顺序播放。
- `agent_voice_state.*`：只描述传输；播放开始/结束不能改写正在发送的下一条。
- `/sdcard/shenyong/` 是当前专用根目录；旧金山 `/sdcard/agent` 与 `/sdcard/rec` 不导入、不自动重放、不删除。
- `font_puhui_assistant_24_4.c` 与 `font_puhui_shenyong_36_4.c` 分别服务文案和主标题/按钮；更改中文文案后重新生成，不加载动态历史字体。

必须运行 guardrails 中全部主机测试、`recorder_concurrency_test`、运行日志判定测试和
ESP-IDF 5.5.3 的 `idf.py build`。真机验证连续两条录音、自动播报、播报中插话和串口无
SPI2/栈/哈希错误。网络与硬件的真实验收不得用纯 reducer 测试代替。

2.2.8 默认通过 Reply-Format 请求 AMR-NB。文件原子发布为 assistant.amr，manifest 中
assistant_path 必须如实记录格式；旧 WAV 不删除并继续兼容。AMR 文件头固定 #!AMR\n，
拒绝保留帧类型/坏填充/截断帧，解码使用已有 Espressif AMR-NB 库，输出再重采样到 24 kHz。
启动时用一个合成静音帧验证目标解码器，不播放测试音；USB 必须看到 self-test: ok。

## 2.2.9 实时 AMR 录音与暗屏保护

新录音为 user.amr：24→16 kHz NS 后实时重采样至 8 kHz，使用 Espressif AMR-NB 12.2 kbps
按 20 ms 帧编码，松手只 flush 与保存摘要，不做整段离线转码。逻辑 PCM 字节数继续用于
原时长上限和 UI 时间；manifest 的 user_path 和格式区分 AMR/WAV，旧 WAV 队列仍可上传。
encoder 流式分帧/尾帧补齐/重置测试和启动目标编码器自检必须通过。

暗屏固定持有一层显示 pause，RecorderDisplayCanResume 必须阻止 SD/录音代码释放这一层。
FlushDisplayThenPause 只能新增自己的 pause，不能预先 Resume；否则黑屏时会启动 LVGL，
与手动触摸/SD 争用并可能引发重启、亮屏和重新联网。息屏没有网络停止操作。

## 2.2.10 启动栈修复

AMR-NB 编码器的 `code_10i40_35bits` 在原 8 KiB main 栈上触发保护异常，表现为持续重启闪屏。
所有录音初始化和主循环移至同一个 24 KiB 专用任务；禁止只移除自检，因为实际录音仍需该栈。
启动编码自检和每条录音保存后输出栈最低余量；真机必须确认两个 codec 自检成功、联网及连续运行。

## 2.2.11 音量键和防误触

实体按键使用 OnPressDown，每次物理按下只改变 10% 音量，无双击反向或 KEY 录音行为。
`DiscardShortRecording` 必须在显示 pause 内、`FinishRecording`/哈希/MarkRecorded 之前调用，
并覆盖普通松手与返回菜单。短录音不改变在途队列，原回复在下一轮自动恢复播放。
边界测试覆盖不足/恰好/超过 1 秒、非法采样率及所有亮暗屏/录音/播放状态下的音量键。

## 2.2.12 发送成功提示音

`recorder_send_cue.*` 与生成的 `recorder_send_cue_pcm.inc` 提供 160 ms 下滑扫频/气流声，
附 80 ms 静音用于 DMA 收尾。成功确认后排队，20 ms 一块；重复确认/缓存 replay 不入队。
主循环在麦克风关闭且录音文件已收尾后播放，优先于回复但保留回复游标，输出使用原音量。
单测验证没有确认不响、重复/重放抑制、录音暂停不丢样本、多条成功排队、幅度/尾静音和重采样。

2.2.12 实机暗屏验收：串口在 accepted 后 20 ms 记录 cue playback start，只有一次播放；
用户确认提示音正常，BOOT/KEY 音量操作继续有效。
