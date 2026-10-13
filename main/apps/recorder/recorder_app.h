#ifndef RECORDER_APP_H_
#define RECORDER_APP_H_

// 神勇语音助手：按住录音、松开发送，后台 FIFO 收发，回复自动播放。
// WAV 和清单保存到 /sdcard/shenyong；主任务协调 SPI2、音频和网络。
// 本函数不返回。
void RunRecorderApp();

#endif  // RECORDER_APP_H_
