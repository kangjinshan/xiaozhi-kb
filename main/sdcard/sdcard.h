#ifndef _SDCARD_H_
#define _SDCARD_H_

// SD 卡（TF 卡）公共挂载模块。
//
// 本板（Waveshare ESP32-C6-Touch-AMOLED-2.16）的 SD 卡走 SPI 模式，
// 与 AMOLED 屏共用 SPI2_HOST 总线（CLK/MOSI/MISO 同引脚，靠 CS 分时复用）。
// 引脚来自官方原理图：CLK=GPIO0、CMD(MOSI)=GPIO1、D0(MISO)=GPIO2、CD/D3(CS)=GPIO6。
//
// 两种启动模式对总线的处理不同：
//   - 小智模式：屏幕初始化时已 spi_bus_initialize(SPI2_HOST)，SD 卡须复用 → own_spi_bus=false
//   - 键盘模式：无屏幕，SPI2_HOST 空 → SD 卡须自建总线 → own_spi_bus=true

// 挂载 SD 卡到 /sdcard。
//   own_spi_bus=true  时函数内部先 spi_bus_initialize(SPI2_HOST)（键盘模式）。
//   own_spi_bus=false 时复用调用方已初始化的 SPI2_HOST（小智模式）。
// 幂等：已挂载时直接返回 true，不重复挂载。
// 返回：挂载成功返回 true；失败（未插卡/需格式化 FAT32/总线错误）返回 false 并打印原因。
bool SdCardMount(bool own_spi_bus);

// 查询当前是否已成功挂载。
bool SdCardIsMounted();

#endif  // _SDCARD_H_
