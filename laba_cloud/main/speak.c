/*
 * speak.c - 文本累积、分句、逐句合成播放
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "audio_out.h"
#include "ble_bridge.h"
#include "cfg_store.h"
#include "speak.h"
#include "volc_tts.h"
#include "wifi_prov.h"

static const char *TAG = "speak";

#define TEXT_MAX            1024        /* 和火山 V1 的 text 上限对齐 */
#define SILENCE_MS          300         /* 多久没新数据就算一句说完 */
#define SENT_MAX            220         /* 单句字节上限，远低于火山 1024 上限 */
#define SENT_MIN_CUT        12          /* 太短的片段不单独成句，避免碎成一堆语气词 */
#define TASK_STACK          12288       /* TLS 握手吃栈，必须给足 */
#define TASK_PRIO           5
#define POLL_MS             50

static char              s_text[TEXT_MAX + 1];
static size_t            s_text_len;
static int64_t           s_last_us;
static SemaphoreHandle_t s_lock;
static volatile bool     s_stop_req;
static volatile bool     s_busy;
static app_cfg_t         s_cfg;

/* ---------------------------- 分句 ---------------------------- */
/* 中文标点都是 3 字节 UTF-8，这里列出常见句读 */
static const char *const PUNCT[] = {
    "。", "，", "！", "？", "；", "、", "：", "…", "—", "）", "》",
};

/* 返回该位置是否标点，并给出字节长度（0 表示不是） */
static size_t punct_len(const char *p, size_t remain)
{
    if (remain == 0) {
        return 0;
    }
    if (*p == '.' || *p == ',' || *p == '!' || *p == '?' || *p == ';' ||
        *p == ':' || *p == '\n') {
        return 1;
    }
    for (size_t i = 0; i < sizeof(PUNCT) / sizeof(PUNCT[0]); i++) {
        size_t pl = strlen(PUNCT[i]);
        if (remain >= pl && memcmp(p, PUNCT[i], pl) == 0) {
            return pl;
        }
    }
    return 0;
}

/* 是否 UTF-8 续字节（0b10xxxxxx） */
static bool is_cont(const char *p)
{
    return ((unsigned char)*p & 0xC0) == 0x80;
}

/*
 * 从 text 里切一句出来。
 * 返回写入 out 的字节数（0 表示没有更多内容），*consumed 为消耗的输入字节数。
 */
static size_t split_sentence(const char *text, size_t len,
                             size_t *consumed, char *out, size_t cap)
{
    size_t i = 0;
    size_t o = 0;

    /* 跳过前导空白 */
    while (i < len && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' ||
                       text[i] == '\t')) {
        i++;
    }
    if (i >= len) {
        *consumed = i;
        return 0;
    }

    while (i < len && o + 4 < cap) {
        size_t pl = punct_len(text + i, len - i);
        if (pl > 0) {
            memcpy(out + o, text + i, pl);
            o += pl;
            i += pl;
            if (o >= SENT_MIN_CUT) {
                break;      /* 够长就断在这里 */
            }
            continue;       /* 太短则连带标点一起继续攒 */
        }
        out[o++] = text[i++];
        if (o >= SENT_MAX) {
            /* 硬断，但不能切在 UTF-8 多字节字符中间 */
            while (i < len && is_cont(text + i)) {
                if (o + 1 >= cap) {
                    break;
                }
                out[o++] = text[i++];
            }
            break;
        }
    }

    out[o] = '\0';
    *consumed = i;
    return o;
}

/* ---------------------------- 音频回调 ---------------------------- */
static esp_err_t on_pcm(const int16_t *pcm, size_t samples, void *user)
{
    (void)user;
    if (s_stop_req) {
        return ESP_FAIL;    /* 主动中断 */
    }
    return audio_out_write_mono16(pcm, samples);
}

/* ---------------------------- 播放一段 ---------------------------- */
static bool speak_one_text(const char *text, size_t len)
{
    if (!wifi_sta_connected()) {
        ESP_LOGE(TAG, "Wi-Fi 未连接，无法合成");
        ble_bridge_notify("ERR:NOWIFI");
        return false;
    }

    size_t off = 0;
    int sentences = 0;
    char sent[SENT_MAX + 8];

    while (off < len) {
        if (s_stop_req) {
            ESP_LOGW(TAG, "收到停止请求，中断剩余句子");
            break;
        }

        size_t consumed = 0;
        size_t n = split_sentence(text + off, len - off, &consumed, sent, sizeof(sent));
        if (n == 0 || consumed == 0) {
            break;
        }
        off += consumed;

        ESP_LOGI(TAG, "第 %d 句 (%u 字节): %.40s", ++sentences, (unsigned)n, sent);
        ble_bridge_notify(sentences == 1 ? "OK:PLAY" : "OK:NEXT");

        volc_result_t r;
        esp_err_t err = volc_tts_speak(&s_cfg, sent, on_pcm, NULL, &r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "第 %d 句合成失败: %s (code=%d)", sentences,
                     esp_err_to_name(err), r.api_code);
            if (r.api_code != 0 && r.api_code != 3000) {
                ble_bridge_notify("ERR:API");
            } else {
                ble_bridge_notify("ERR:NET");
            }
            return false;
        }
    }

    audio_out_drain();
    return true;
}

/* ---------------------------- 任务 ---------------------------- */
static void speak_task(void *arg)
{
    (void)arg;
    char local[TEXT_MAX + 1];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (s_text_len == 0) {
            continue;
        }
        if ((esp_timer_get_time() - s_last_us) < (int64_t)SILENCE_MS * 1000) {
            continue;       /* 用户还在发，等他说完 */
        }

        /* 取走整段文本 */
        size_t len = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        len = s_text_len;
        memcpy(local, s_text, len);
        local[len] = '\0';
        s_text_len = 0;
        xSemaphoreGive(s_lock);

        if (len == 0) {
            continue;
        }

        s_stop_req = false;
        s_busy = true;
        ESP_LOGI(TAG, "开始处理 %u 字节文本", (unsigned)len);

        bool ok = speak_one_text(local, len);

        s_busy = false;
        if (ok && !s_stop_req) {
            ble_bridge_notify("OK:DONE");
        } else if (s_stop_req) {
            ble_bridge_notify("OK:STOP");
        }
        s_stop_req = false;
    }
}

/* ---------------------------- 对外接口 ---------------------------- */
esp_err_t speak_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cfg_load(&s_cfg);
    if (err != ESP_OK) {
        cfg_set_defaults(&s_cfg);
    }

    BaseType_t ok = xTaskCreate(speak_task, "speak", TASK_STACK, NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void speak_feed(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_text_len + len > TEXT_MAX) {
        ESP_LOGW(TAG, "文本溢出（已有 %u + 新来 %u > %d），丢弃旧内容重新累积",
                 (unsigned)s_text_len, (unsigned)len, TEXT_MAX);
        s_text_len = 0;
    }
    if (len > TEXT_MAX) {
        data += (len - TEXT_MAX);
        len = TEXT_MAX;
    }
    memcpy(s_text + s_text_len, data, len);
    s_text_len += len;
    s_last_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void speak_stop(void)
{
    s_stop_req = true;
    audio_out_stop();
}

esp_err_t speak_say(const char *text)
{
    if (text == NULL || text[0] == '\0' || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(text);
    if (len > TEXT_MAX) {
        len = TEXT_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_text, text, len);
    s_text_len = len;
    /* 把「最后写入时间」拨到静默窗口之前，任务下一轮立刻取走 */
    s_last_us = esp_timer_get_time() - (int64_t)(SILENCE_MS + 50) * 1000;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

bool speak_busy(void)
{
    return s_busy;
}

void speak_reload_cfg(void)
{
    app_cfg_t tmp;
    if (cfg_load(&tmp) == ESP_OK) {
        s_cfg = tmp;
        ESP_LOGI(TAG, "配置已重新加载: voice=%s speed=%u vol=%u",
                 s_cfg.voice, s_cfg.speed_x100, s_cfg.volume_x100);
    }
}
