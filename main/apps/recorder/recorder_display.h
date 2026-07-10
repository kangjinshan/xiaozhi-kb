#ifndef RECORDER_DISPLAY_H_
#define RECORDER_DISPLAY_H_

#include <driver/i2c_master.h>
#include <esp_err.h>

// 录音 app 的独立屏幕显示（AMOLED SH8601 + LVGL）。
// 复用键盘模式 keyboard_zone_display 的独立屏幕初始化思路：无小智 Board 也能点屏。
//
// 屏与 SD 卡共用 SPI2_HOST：本模块的 SPI 总线初始化容忍 ESP_ERR_INVALID_STATE（幂等），
// 正确用法是先调 RecorderDisplayInit()（建总线）再 SdCardMount(false)（复用总线）。

// 初始化屏幕。pmic 是已初始化的 AXP2101 i2c 设备句柄（屏上电时序需要）。
// 幂等：重复调用只初始化一次。
esp_err_t RecorderDisplayInit(i2c_master_dev_handle_t pmic);

// 在屏幕居中显示一大一小两行文字（title 大字，subtitle 小字，可为 nullptr）。
// 线程安全（内部加 lvgl 锁）。未初始化时无操作。
void RecorderShowText(const char* title, const char* subtitle);

#endif  // RECORDER_DISPLAY_H_
