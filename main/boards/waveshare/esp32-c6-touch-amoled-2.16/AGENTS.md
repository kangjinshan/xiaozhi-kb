# Waveshare ESP32-C6 Touch AMOLED 2.16 板级开发指南

> 最后更新：2026-07-11
> 位置：`main/boards/waveshare/esp32-c6-touch-amoled-2.16/`

## 1. 概述

该目录把通用小智运行时绑定到 Waveshare ESP32-C6 2.16 英寸 AMOLED 板，负责 AXP2101 电源、SH8601 QSPI 屏、CST9217 触摸、ES8311/ES7210 音频、物理按键和共享 SPI2 上的 SD 卡。

## 2. 核心代码结构

| 文件 | 职责 | 关键类/方法 |
| --- | --- | --- |
| `esp32-c6-touch-amoled-2.16.cc` | 板实例和外设初始化 | `WaveshareEsp32c6TouchAMOLED2inch16`、`InitializeSH8601Display()`、`InitializeTouch()` |
| `config.h` | GPIO、分辨率、音频参数 | `LCD_*`、`AUDIO_*`、`BOOT_BUTTON_GPIO`、`KEY_LEFT_GPIO` |
| `config.json` | ESP-IDF 目标和板型构建元数据 | `target=esp32c6` |
| `README.md` | 上游板型选择和基础构建说明 | `idf.py set-target/menuconfig/build` |

## 3. 初始化流程

`app_main()` → `Board::GetInstance()` → 板构造函数：

1. `InitializePowerSaveTimer()`
2. `InitializeCodecI2c()` 和 `InitializeAxp2101()`
3. `InitializeSpi()` 和 `InitializeSH8601Display()`
4. 停止 LVGL、取得 LVGL 锁、`SdCardMount(false)`、解锁并恢复 LVGL
5. `InitializeTouch()`、`InitializeButtons()`、`InitializeTools()`

不得把第 4 步恢复为无锁挂载；SH8601 首帧和 SDSPI 初始化会同时操作 SPI2 硬件。

## 4. 硬件与并发约束

- AMOLED QSPI 与 SD SPI 共用 SPI2：CLK=GPIO0、LCD D0/SD MOSI=GPIO1、LCD D1/SD MISO=GPIO2；LCD CS=GPIO15，SD CS=GPIO6。
- GPIO12/GPIO13 是 USB D-/D+，禁止扫描或初始化为普通 GPIO。
- GPIO9 是 BOOT，GPIO10 是左键。中间 PWR 键接 AXP2101，长按可能硬件关机。
- SH8601 无 reset GPIO，通过 `Pmic_SetAldo3()` 执行供电复位。
- `CustomLcdDisplay::rounder_event_cb()` 必须把无效区域扩展到偶数起点、奇数终点，满足 SH8601 刷新区域要求。
- 小智模式只挂载 SD，不调用 `SdCardLogStart()`。同步日志 hook 会在 Wi-Fi `sys_evt` 任务的 2304 字节栈内执行 FATFS 写入并造成 `Stack protection fault`。
- 录音模式有自己的显示和 SD 初始化路径；录音、Agent 上传读取、回复分块落卡、SHA-256 扫描和清单更新全部在主任务中使用 `RecorderDisplayPause()` / `RecorderDisplayResume()` 协调 SPI2。网络回调只能复制有界事件，不能碰 FATFS 或 LVGL。

## 5. 已确认故障与诊断

### 选择器持续闪屏

- 串口：`assert failed: spi_hal_setup_trans ... spi_ll_get_running_cmd(hw) == 0`
- 根因：LVGL 首帧和 `esp_vfs_fat_sdspi_mount()` 并发使用 SPI2。
- 修复：挂载期间 `lvgl_port_stop()` + `lvgl_port_lock()`。
- 验证：`scripts/verify_selector_stability.py --duration 6`。

### 小智 AI 重复加载

- 串口：Wi-Fi 扫描到 AP 后 `Detected in task "sys_evt"`、`Stack protection fault`。
- 回溯：`SdCardLogVprintf()` → `vfs_fat_write()` / `ff_mutex_take()`。
- 修复：板构造函数不安装 SD 日志 hook；日志走 USB 串口。
- 验证：`scripts/verify_xiaozhi_stability.py --duration 15`。

## 6. 常见修改场景

- 调整按键：修改 `config.h` 和 `InitializeButtons()`，同时检查 BOOT 下载模式与 PWR 硬件行为。
- 调整屏幕方向/区域：修改 `DISPLAY_*` 或 `rounder_event_cb()`，真机检查完整刷新和局部刷新。
- 调整 SD 初始化：保持 LVGL 停止和锁覆盖整个 `SdCardMount(false)`；失败路径也必须恢复 LVGL。
- 恢复小智 SD 日志：禁止直接调用现有 `SdCardLogStart()`；应先把日志写入改为有界队列 + 独立大栈任务，再验证丢弃策略和 SPI2 仲裁。

## 7. 回归要求

```bash
source ~/esp/esp-idf/export.sh
idf.py build
/tmp/ser-venv/bin/python scripts/verify_selector_stability.py --duration 6
/tmp/ser-venv/bin/python scripts/verify_xiaozhi_stability.py --duration 15
```

修改复位、OTA、NVS 或 USB 相关代码后还要做物理冷启动验证。自动复位失败时：拔 USB 和电池，等待 20 秒后不按 BOOT 重新上电。
