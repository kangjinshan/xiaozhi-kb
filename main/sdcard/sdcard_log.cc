#include "sdcard_log.h"
#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "sdcard_log"

static const char* kLogDir = "/sdcard/log";
static const int kFlushEveryLines = 20;   // 每 N 行 fflush 一次，兼顾性能与断电数据安全
static const size_t kLineBufSize = 512;   // 单行格式化缓冲（超长行截断，可接受）

static FILE* s_log_file = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t s_in_hook_task = nullptr;  // 尽力而为的重入保护（单核）
static int s_line_count = 0;
static vprintf_like_t s_prev_vprintf = nullptr;  // 保存原 hook，未挂载时回退
static bool s_started = false;

// 去除 ANSI 转义序列（ESP_LOG 的颜色码 \033[...m），让落盘日志文件可读。
// 原地压缩，返回新长度。
static size_t StripAnsi(char* s, size_t len) {
    size_t w = 0;
    for (size_t r = 0; r < len;) {
        if (s[r] == '\033') {  // ESC，跳过直到序列结束字母（通常 'm'）
            r++;
            if (r < len && s[r] == '[') {
                r++;
                while (r < len && !(s[r] >= '@' && s[r] <= '~')) {
                    r++;
                }
                if (r < len) {
                    r++;  // 跳过结束字母
                }
            }
            continue;
        }
        s[w++] = s[r++];
    }
    return w;
}

// 扫描 /sdcard/log 目录，返回下一个可用的 bootN.log 序号（现有最大 N + 1）。
static int NextBootIndex() {
    int max_idx = -1;
    DIR* dir = opendir(kLogDir);
    if (dir != nullptr) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            int idx = -1;
            if (sscanf(ent->d_name, "boot%d.log", &idx) == 1 && idx > max_idx) {
                max_idx = idx;
            }
        }
        closedir(dir);
    }
    return max_idx + 1;
}

// 自定义 vprintf hook：始终串口输出；SD 卡已挂载时同时写文件（去色、限流、防重入）。
static int SdCardLogVprintf(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vprintf(fmt, args);  // 串口原样输出（保留颜色），不改变原行为

    if (s_log_file != nullptr && SdCardIsMounted()) {
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        // 防重入：本任务已在写文件（VFS/FAT 内部若产生日志会递归回到这里）时直接跳过
        if (s_in_hook_task != self &&
            xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_in_hook_task = self;

            char buf[kLineBufSize];
            int len = vsnprintf(buf, sizeof(buf), fmt, copy);
            if (len > 0) {
                size_t clean = StripAnsi(buf, len < (int)sizeof(buf) ? len : sizeof(buf) - 1);
                fwrite(buf, 1, clean, s_log_file);
                if (++s_line_count >= kFlushEveryLines) {
                    fflush(s_log_file);
                    s_line_count = 0;
                }
            }

            s_in_hook_task = nullptr;
            xSemaphoreGive(s_mutex);
        }
    }

    va_end(copy);
    return n;
}

void SdCardLogStart() {
    if (s_started) {
        return;  // 幂等
    }
    if (!SdCardIsMounted()) {
        ESP_LOGW(TAG, "SD 卡未挂载，日志落盘未启用（仅串口输出）");
        return;
    }

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == nullptr) {
            ESP_LOGE(TAG, "创建互斥锁失败，日志落盘未启用");
            return;
        }
    }

    mkdir(kLogDir, 0777);  // 目录不存在则创建（已存在返回错误，忽略）

    int idx = NextBootIndex();
    char path[64];
    snprintf(path, sizeof(path), "%s/boot%d.log", kLogDir, idx);
    s_log_file = fopen(path, "w");
    if (s_log_file == nullptr) {
        ESP_LOGE(TAG, "无法创建日志文件 %s，日志落盘未启用", path);
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(SdCardLogVprintf);
    s_started = true;
    ESP_LOGI(TAG, "日志落盘已启用 -> %s", path);
}
