#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <esp_log.h>

#define TAG "sdcard"

// SD 卡引脚与总线（来自官方原理图，已硬件验证）。
// 与 AMOLED 屏共用 SPI2_HOST：小智模式复用屏幕已建总线，键盘模式自建。
static const spi_host_device_t kSdSpiHost = SPI2_HOST;
static const gpio_num_t kSdPinClk = GPIO_NUM_0;   // 与 LCD_PCLK 同引脚
static const gpio_num_t kSdPinMosi = GPIO_NUM_1;  // 与 LCD_D0 同引脚
static const gpio_num_t kSdPinMiso = GPIO_NUM_2;  // 与 LCD_D1 同引脚
static const gpio_num_t kSdPinCs = GPIO_NUM_6;    // SD 卡独立片选
static const char* kSdMountPoint = "/sdcard";

static sdmmc_card_t* s_card = nullptr;  // 非空表示已挂载

bool SdCardIsMounted() {
    return s_card != nullptr;
}

// 挂载后读写自检：写一个测试文件→读回→比对，全程打印串口。
// 让 SD 卡的读写通路无需拔卡即可从串口日志端到端确认。
void SdCardSelfTest() {
    const char* path = "/sdcard/.sdtest";
    const char* content = "xiaozhi-sdcard-ok";

    FILE* fw = fopen(path, "w");
    if (fw == nullptr) {
        ESP_LOGE(TAG, "自检失败：无法写入 %s（卡只读或损坏？）", path);
        return;
    }
    size_t written = fwrite(content, 1, strlen(content), fw);
    fclose(fw);

    char buf[64] = {0};
    FILE* fr = fopen(path, "r");
    if (fr == nullptr) {
        ESP_LOGE(TAG, "自检失败：写入成功但无法读回 %s", path);
        return;
    }
    size_t read = fread(buf, 1, sizeof(buf) - 1, fr);
    fclose(fr);
    remove(path);  // 清理测试文件

    if (written == strlen(content) && read == strlen(content) &&
        strcmp(buf, content) == 0) {
        ESP_LOGI(TAG, "自检通过：写入并读回一致（读写正常）");
    } else {
        ESP_LOGW(TAG, "自检异常：写=%u 读=%u 内容='%s'", (unsigned)written,
                 (unsigned)read, buf);
    }
}

bool SdCardMount(bool own_spi_bus) {
    if (s_card != nullptr) {
        return true;  // 幂等：已挂载
    }

    // 键盘模式无屏幕，SPI2_HOST 为空，需自建总线；小智模式复用屏幕已建总线。
    if (own_spi_bus) {
        spi_bus_config_t bus_cfg = {};
        bus_cfg.sclk_io_num = kSdPinClk;
        bus_cfg.mosi_io_num = kSdPinMosi;
        bus_cfg.miso_io_num = kSdPinMiso;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4096;
        esp_err_t err = spi_bus_initialize(kSdSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
            return false;
        }
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSdSpiHost;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = kSdPinCs;
    slot_config.host_id = kSdSpiHost;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // 不自动格式化，避免误删用户数据
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t ret =
        esp_vfs_fat_sdspi_mount(kSdMountPoint, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        s_card = nullptr;
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "SD 卡挂载失败：文件系统无法识别（未插卡或需格式化为 FAT32）");
        } else {
            ESP_LOGW(TAG, "SD 卡初始化失败: %s（检查是否插卡）", esp_err_to_name(ret));
        }
        // 自建总线时挂载失败，回收总线，便于后续重试或让位其它用途。
        if (own_spi_bus) {
            spi_bus_free(kSdSpiHost);
        }
        return false;
    }

    ESP_LOGI(TAG, "SD 卡挂载成功 -> %s", kSdMountPoint);
    sdmmc_card_print_info(stdout, s_card);  // 打印容量、类型等卡信息到串口
    SdCardSelfTest();
    return true;
}
