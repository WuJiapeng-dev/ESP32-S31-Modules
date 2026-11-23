/*
 * tts.c - 播放编排层
 *
 * 数据流：BLE 写回调 -> tts_feed()（只拷贝）-> tts 任务（拼接/转码/播放/回执）
 * 放到独立任务里做，是因为等 BY 忙信号可能要十几秒，不能卡住 NimBLE 协议栈。
 */
#include "tts.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_bridge.h"
#include "gb2312.h"
#include "syn6288.h"

static const char *TAG = "tts";

#define TTS_IDLE_FLUSH_US   (250 * 1000)    /* 静默 250ms 视为一句话结束 */
#define TTS_TASK_STACK      4096
#define TTS_TASK_PRIO       5

static char         s_buf[TTS_TEXT_MAX];
static size_t       s_len;
static int64_t      s_last_us;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* BLE 侧入口：只做拷贝，立即返回                                      */
/* ------------------------------------------------------------------ */
void tts_feed(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_lock);

    size_t space = TTS_TEXT_MAX - 1 - s_len;
    if (len > space) {
        len = space;
    }
    if (len > 0) {
        memcpy(s_buf + s_len, data, len);
        s_len += len;
        s_buf[s_len] = '\0';
        s_last_us = esp_timer_get_time();
    }

    portEXIT_CRITICAL(&s_lock);
}

/* 取出一句完整的话：静默超过阈值，或缓冲已满 */
static bool take_pending(char *out, size_t out_size, size_t *out_len)
{
    bool got = false;

    portENTER_CRITICAL(&s_lock);

    if (s_len > 0) {
        int64_t idle_us = esp_timer_get_time() - s_last_us;
        if (idle_us >= TTS_IDLE_FLUSH_US || s_len >= TTS_TEXT_MAX - 1) {
            size_t n = s_len;
            if (n > out_size - 1) {
                n = out_size - 1;
            }
            memcpy(out, s_buf, n);
            out[n] = '\0';

            s_len = 0;
            s_buf[0] = '\0';
            *out_len = n;
            got = true;
        }
    }

    portEXIT_CRITICAL(&s_lock);
    return got;
}

/* ------------------------------------------------------------------ */
/* 一句话的处理：转码 -> 播放 -> 回执                                  */
/* ------------------------------------------------------------------ */
static void handle_text(const char *utf8, size_t len)
{
    /* static：避免在 4KB 任务栈上开大数组 */
    static uint8_t gb[TTS_TEXT_MAX + 8];

    size_t skipped = 0;
    size_t gb_len = gb2312_from_utf8(utf8, gb, sizeof(gb), &skipped);

    if (gb_len == 0) {
        ESP_LOGW(TAG, "没有可转换的字符（跳过 %u 个），不播报", (unsigned)skipped);
        ble_bridge_notify("ERR:NOGB");
        return;
    }

    ESP_LOGI(TAG, "转码: UTF-8 %u 字节 -> GB2312 %u 字节，跳过 %u 个字符",
             (unsigned)len, (unsigned)gb_len, (unsigned)skipped);

    int64_t t0 = esp_timer_get_time();
    size_t frames = 0;

    esp_err_t err = syn6288_play_gb2312(gb, gb_len, &frames);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "播放命令发送失败: %s", esp_err_to_name(err));
        ble_bridge_notify("ERR:UART");
        return;
    }

    /* 48 字节：容纳两个 10 位数的极值组合，避免 -Werror=format-truncation；
     * 实际发出的内容都 <= 20 字节（受 Notify 单包上限约束） */
    char msg[48];
    if (skipped > 0) {
        snprintf(msg, sizeof(msg), "OK:PLAY %uB skip%u",
                 (unsigned)gb_len, (unsigned)skipped);
    } else {
        snprintf(msg, sizeof(msg), "OK:PLAY %uB", (unsigned)gb_len);
    }
    ble_bridge_notify(msg);

    /* 模块收到命令会回传一个字节：0x41=命令正确，0x45=命令错误 */
    uint8_t reply[8];
    size_t rn = syn6288_read_reply(reply, sizeof(reply), 80);
    if (rn > 0) {
        snprintf(msg, sizeof(msg), "ACK:%02X", reply[0]);
        ble_bridge_notify(msg);
    }

    /* 等 BY 变忙：模块没供电 / BY 没接线时这里会失败，先回报，避免后面白等 */
    if (!syn6288_wait_busy(800)) {
        ESP_LOGW(TAG, "800ms 内未检测到 BY 忙信号，请检查 GPIO%d 与模块 BY 的接线",
                 SYN6288_BY_GPIO);
        ble_bridge_notify("WARN:NOBY");
        return;
    }

    /* 播报时长上限：每 2 字节（1 个汉字）按 500ms 估，最少 5s，最多 120s */
    uint32_t est_chars = (uint32_t)(gb_len / 2) + 1;
    uint32_t timeout = 5000 + est_chars * 500;
    if (timeout > 120000) {
        timeout = 120000;
    }

    if (!syn6288_wait_idle(timeout)) {
        ESP_LOGW(TAG, "等待播报结束超时（上限 %u ms）", (unsigned)timeout);
        ble_bridge_notify("WARN:TIMEOUT");
        return;
    }

    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    snprintf(msg, sizeof(msg), "OK:DONE %ums", (unsigned)ms);
    ble_bridge_notify(msg);

    ESP_LOGI(TAG, "播报完成，用时 %u ms，共 %u 帧", (unsigned)ms, (unsigned)frames);
}

static void tts_task(void *arg)
{
    (void)arg;

    char text[TTS_TEXT_MAX];

    ESP_LOGI(TAG, "播放任务启动（静默 %d ms 视为一句话结束）", TTS_IDLE_FLUSH_US / 1000);

    for (;;) {
        size_t n = 0;
        if (take_pending(text, sizeof(text), &n)) {
            handle_text(text, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

esp_err_t tts_init(void)
{
    s_len = 0;
    s_buf[0] = '\0';
    s_last_us = esp_timer_get_time();

    if (xTaskCreate(tts_task, "tts", TTS_TASK_STACK, NULL, TTS_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建播放任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
