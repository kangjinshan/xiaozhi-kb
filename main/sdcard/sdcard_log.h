#ifndef _SDCARD_LOG_H_
#define _SDCARD_LOG_H_

// SD 卡日志落盘：把系统 ESP_LOG 输出在保留串口打印的同时，追加写入 SD 卡文件。
//
// 通过 esp_log_set_vprintf 安装一个自定义 vprintf hook：
//   - 始终先原样 vprintf 到串口（不改变原有串口日志行为）
//   - 若 SD 卡已挂载，再把同一行写入 /sdcard/log/bootN.log
// 每次开机新建一个递增序号的日志文件，避免单文件无限增长。
// 写入采用缓冲 + 定期 flush 限流；写失败自动降级为只输出串口。
//
// 前置条件：调用前 SD 卡须已挂载（SdCardMount 成功）。小智/键盘两模式各自在
// 挂载成功后调用本函数，即可让该模式的日志落盘。

// 安装日志落盘 hook 并新建本次开机的日志文件。
// 幂等：重复调用只安装一次。SD 卡未挂载时不安装（返回后日志仍只走串口）。
void SdCardLogStart();

#endif  // _SDCARD_LOG_H_
