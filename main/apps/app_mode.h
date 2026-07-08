#ifndef APP_MODE_H_
#define APP_MODE_H_

enum class AppMode { kSelector, kXiaozhi, kKeyboard };

// 读取当前应用模式；NVS 无值或值非法时返回 kSelector。
AppMode AppModeRead();

// 写入模式到 NVS 并立即软重启（此函数不返回）。
void AppModeWriteAndReboot(AppMode mode);

#endif  // APP_MODE_H_
