#ifndef RECORDER_APP_H_
#define RECORDER_APP_H_

// 独立的 SD 卡录音 app。开机进入后：初始化屏幕/音频/SD 卡，
// 用最左键（KEY_LEFT_GPIO）单击开始/停止录音，录音写为 WAV 到 /sdcard/rec/。
// 本函数不返回（内部死循环）。
void RunRecorderApp();

#endif  // RECORDER_APP_H_
