#ifndef RECORDER_APP_H_
#define RECORDER_APP_H_

// 神勇语音助手：按住录音、松开发送，后台 FIFO 收发，回复自动播放。
// AMR 和清单保存到 /sdcard/shenyong；独立任务协调 SPI2、音频和网络。
// 启动后调用者应返回，释放 ESP-IDF main task 的 8 KiB 栈。
bool StartRecorderApp();

// 仅由专用录音任务调用，本函数不返回。
void RunRecorderApp();

#endif  // RECORDER_APP_H_
