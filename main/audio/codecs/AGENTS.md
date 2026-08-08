# audio/codecs 基础设施开发指南

> 最后更新：2026-08-08
> 位置：`main/audio/codecs/`

## 1. 概述

本目录把公共 `AudioCodec` 接口适配到不同板载 codec 或纯 I2S 设备。实现负责创建 I2S channel、构造 `esp_codec_dev` 控制/数据接口、开关输入输出并映射音量/增益，不处理唤醒词、降噪或应用状态。

## 2. 核心组件

- `BoxAudioCodec`：ES8311 输出 + ES7210 四路 TDM 输入；支持可选输入 channel mask，并由 `SetInputGain()` 同时设置所选物理麦克风。
- `Es8311AudioCodec`：单 ES8311 双工实现，`UpdateDeviceState()` 协调 PA 和 codec 开关。
- `Es8374AudioCodec`、`Es8388AudioCodec`、`Es8389AudioCodec`：分别适配对应 codec 的 I2C 控制和 duplex I2S。
- `NoAudioCodecDuplex`、`NoAudioCodecSimplex`、`NoAudioCodecSimplexPdm`：没有外部 codec 控制面的 STD/PDM I2S 实现。
- `DummyAudioCodec`：无硬件读写的占位实现。
- 所有实现最终通过 `Read()` / `Write()` 被 `AudioCodec::InputData()` / `OutputData()` 调用。

## 3. 设计约定

- `input_sample_rate_`、`output_sample_rate_` 和 I2S clock/slot 必须一致；`BoxAudioCodec::CreateDuplexChannels()` 明确要求输入输出采样率相同。
- `BoxAudioCodec` 的 `input_channel_mask=0` 保持历史行为：默认 MIC1，`input_reference=true` 时再加入 MIC2。传入显式 mask 时，`input_channels_` 等于 mask bit 数。
- `esp_codec_dev_sample_info_t.channel` 表示总 TDM channel 数，`channel_mask` 表示筛选的物理 slot；不可把二者都改成活动声道数。
- 物理映射为 channel 0→ES7210 MIC1、channel 1→MIC2、channel 2→MIC3、channel 3→MIC4；输入增益必须使用相同 mask。
- 开关 input/output 时持有各实现的 `data_if_mutex_`；应用不得绕过 `EnableInput()` 直接 open/close device。
- `BoxAudioCodec::Read()` / `Write()` 只能在底层 `esp_codec_dev_*` 成功时返回请求样本数；设备未启用或 I/O 失败必须返回 0，禁止把旧缓冲区误报为新音频并让上层忙循环。
- 公共构造器参数变化必须保留默认值或同步所有板型调用点，不能只验证当前 Waveshare 板。

## 4. 典型调用路径

```cpp
constexpr uint16_t stereo =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
BoxAudioCodec codec(bus, 24000, 24000, mclk, bclk, ws, dout, din,
                    pa, es8311_addr, es7210_addr, false, stereo);
codec.EnableInput(true);
codec.SetInputGain(30.0f);
codec.InputData(interleaved_pcm);
```

Mic 应用依赖筛选后的 MIC1/MIC2 数据按 L/R 交织返回；金山 AI 与小智调用未传 mask，必须保持原 mono/参考声道行为。

## 5. 维护风险

| 风险 | 概率 | 影响 | 缓解 |
| --- | --- | --- | --- |
| channel 与 mask 语义混淆 | 中 | 高：DMA 布局错误或左右串音 | 对照 `esp_codec_dev_sample_info_t` 与 ES7210 映射，真机单侧验证 |
| runtime 增益只更新一支麦克风 | 中 | 高：立体声偏移 | `SetInputGain()` 使用 `input_channel_mask_` |
| 构造器签名破坏其他板型 | 中 | 高：其他 target 编译失败 | 新参数提供兼容默认值并搜索全部构造调用 |
| close/open 与数据读写并发 | 低 | 高：driver 状态错误 | 保持 `data_if_mutex_` 和应用主任务串行化 |

修改本目录实现后，至少构建当前 ESP32-C6 目标；涉及 `BoxAudioCodec` 输入行为时还要运行 Recorder 防回归并真机检查 Mic 双声道。
