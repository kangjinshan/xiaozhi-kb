#ifndef RECORDER_DISPLAY_H_
#define RECORDER_DISPLAY_H_

#include <driver/i2c_master.h>
#include <esp_err.h>

#include <cstddef>
#include <string>
#include <vector>

struct RecorderDisplayMenuItem {
    std::string label;
    std::string detail;
    std::string path;
};

using RecorderDisplayCallback = void (*)(void* user_data);
using RecorderDisplayFileCallback = void (*)(const char* path, void* user_data);

// 录音 app 的独立屏幕显示（AMOLED SH8601 + LVGL）。
// 复用键盘模式 keyboard_zone_display 的独立屏幕初始化思路：无小智 Board 也能点屏。
//
// 屏与 SD 卡共用 SPI2_HOST：本模块的 SPI 总线初始化容忍 ESP_ERR_INVALID_STATE（幂等），
// 正确用法是先调 RecorderDisplayInit()（建总线）再 SdCardMount(false)（复用总线）。

// 初始化屏幕与触摸。pmic 是已初始化的 AXP2101 i2c 设备句柄（屏上电时序需要）。
// 幂等：重复调用只初始化一次。
esp_err_t RecorderDisplayInit(i2c_master_bus_handle_t i2c_bus, i2c_master_dev_handle_t pmic);

// 注册屏幕事件回调。open_menu 点击 PLAY 菜单入口时触发；play_file 点击某个录音文件时触发。
void RecorderDisplaySetCallbacks(RecorderDisplayCallback open_menu,
                                 RecorderDisplayFileCallback play_file,
                                 void* user_data);

// 在屏幕居中显示一大一小两行文字（title 大字，subtitle 小字，可为 nullptr）。
// 线程安全（内部加 lvgl 锁）。未初始化时无操作。
void RecorderShowText(const char* title, const char* subtitle);

// 控制 PLAY 菜单入口。录音中/播放中应隐藏，保存成功或空闲时可显示。
void RecorderDisplaySetPlayMenuVisible(bool visible);

// 屏和 SD 卡共用 SPI2。执行 SD 大块读写前暂停 LVGL 刷屏，结束后恢复。
// 暂停期间同一任务仍可调用 RecorderShowText/RecorderDisplayShowFileMenu 更新对象；
// 恢复后再统一刷新到屏幕。
void RecorderDisplayPause();
void RecorderDisplayResume();

// 展示或隐藏录音文件菜单。items 由调用方扫描 SD 卡后传入。
void RecorderDisplayShowFileMenu(const std::vector<RecorderDisplayMenuItem>& items);
void RecorderDisplayHideFileMenu();

#endif  // RECORDER_DISPLAY_H_
