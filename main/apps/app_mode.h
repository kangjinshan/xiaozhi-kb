#ifndef APP_MODE_H_
#define APP_MODE_H_

#include "keyboard_touch_action.h"

enum class AppMode { kSelector, kXiaozhi, kKeyboard, kRecorder };

// 读取当前应用模式；NVS 无值或值非法时返回 kSelector。
AppMode AppModeRead();

// 读取蓝牙键盘配置；NVS 无值或值非法时返回配置1。
KeyboardProfile KeyboardProfileRead();

// 写入模式到 NVS 并立即软重启（此函数不返回）。
void AppModeWriteAndReboot(AppMode mode);

// 写入键盘配置，切到蓝牙键盘模式并立即软重启（此函数不返回）。
void AppModeWriteKeyboardAndReboot(KeyboardProfile profile);

#endif  // APP_MODE_H_
