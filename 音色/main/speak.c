/*
 * speak.c - 文本累积、切块、逐块合成播放
 *
 * 为什么还要切块：
 *   DeepSeek 的 TTS 是边合成边返回音频的，本来一整段丢进去最省事（中间没有断点）。
 *   但单次请求有 10000 计费字符 / 64 KiB 的上限，而且一次失败会丢掉整段。
 *   所以按 DS_TEXT_MAX（1024 字节，约 340 汉字）切块，切点优先落在标点上，
 *   正常一两句话的输入只会切成一块，听感上是连续的一段。
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
#include "ds_tts.h"
#include "speak.h"
#include "wifi_prov.h"

static const char *TAG = "speak";

#define TEXT_MAX            DS_TEXT_MAX     /* 一次累积的上限，与单次请求上限一致 */
#define SILENCE_MS          300             /* 多久没新数据就算一句说完 */
#define TASK_STACK          12288           /* TLS 握手吃栈，必须给足 */
#define TASK_PRIO           5
#define POLL_MS             50

static char              s_text[TEXT_MAX + 1];
static size_t            s_text_len;
static int64_t           s_last_us;
static SemaphoreHandle_t s_lock;
static volatile bool     s_stop_req;
static volatile bool     s_busy;
static app_cfg_t         s_cfg;

/* ---------------------------- 切块 ---------------------------- */
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
 * 决定这一块切多少字节。
 * len 不超过 cap 就直接全要；否则优先取 cap 之前最后一个标点（且不能太靠前），
 * 找不到合适标点就硬切，但必须退到 UTF-8 字符边界上。
 */
static size_t pick_cut(const char *text, size_t len, size_t cap)
{
    if (len <= cap) {
        return len;
    }

    size_t cut = 0;
    size_t i = 0;
    while (i < cap) {
        size_t pl = punct_len(text + i, len - i);
        if (pl > 0 && i + pl <= cap) {
            if (i >= cap / 3) {
                cut = i + pl;       /* 记下最后一个够靠后的标点 */
            }
            i += pl;
        } else {
            i++;
        }
    }
    if (cut > 0) {
        return cut;
    }

    cut = cap;
    while (cut > 1 && is_cont(text + cut)) {
        cut--;                      /* 不能切在多字节字符中间 */
    }
    return cut;
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

/* 把 HTTP 状态码翻译成手机端看得懂的提示 */
static void notify_http_error(int status)
{
    switch (status) {
    case 401:
    case 403:
        ble_bridge_notify("ERR:KEY");       /* key 不对或没权限 */
        break;
    case 400:
        ble_bridge_notify("ERR:TEXT");      /* 文本为空或没过内容审核 */
        break;
    case 429:
        ble_bridge_notify("ERR:BUSY");      /* 并发请求超限 */
        break;
    case 503:
    case 504:
        ble_bridge_notify("ERR:SRV");       /* 服务忙 / 合成超时 */
        break;
    default:
        ble_bridge_notify("ERR:NET");
        break;
    }
}

/* ---------------------------- 播放一段 ---------------------------- */
static bool speak_one_text(const char *text, size_t len)
{
    if (!wifi_sta_connected()) {
        ESP_LOGE(TAG, "Wi-Fi 未连接，无法合成");
        ble_bridge_notify("ERR:NOWIFI");
        return false;
    }
    if (!cfg_ready(&s_cfg)) {
        ESP_LOGE(TAG, "当前引擎的凭据未配置");
        ble_bridge_notify("ERR:NOKEY");
        return false;
    }
    if (cfg_engine_is_doubao(&s_cfg)) {
        /*
         * 豆包（火山 V1）是非流式接口：整段音频以 base64 塞在 JSON 里一次返回，
         * 24kHz/16bit 单声道约 48KB/秒，10 秒就是 640KB base64，ESP32 堆吃不消。
         * 真要接得先定分块策略（每块只放一两句）或换 v3 流式接口，
         * 在那之前明确返回错误码，别让它安静地失败。
         */
        ESP_LOGE(TAG, "豆包引擎后端尚未接入，请切回 DeepSeek");
        ble_bridge_notify("ERR:ENGINE");
        return false;
    }

    size_t off = 0;
    int chunks = 0;
    char buf[DS_TEXT_MAX + 1];

    while (off < len) {
        if (s_stop_req) {
            ESP_LOGW(TAG, "收到停止请求，中断剩余内容");
            break;
        }

        /* 跳过块首空白 */
        while (off < len && (text[off] == ' ' || text[off] == '\n' ||
                             text[off] == '\r' || text[off] == '\t')) {
            off++;
        }
        if (off >= len) {
            break;
        }

        size_t n = pick_cut(text + off, len - off, DS_TEXT_MAX);
        if (n == 0) {
            break;
        }
        memcpy(buf, text + off, n);
        buf[n] = '\0';
        off += n;

        ESP_LOGI(TAG, "第 %d 块 (%u 字节): %.40s", ++chunks, (unsigned)n, buf);
        ble_bridge_notify(chunks == 1 ? "OK:PLAY" : "OK:NEXT");

        ds_result_t r;
        esp_err_t err = ds_tts_speak(&s_cfg, buf, on_pcm, NULL, &r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "第 %d 块合成失败: %s (http=%d)",
                     chunks, esp_err_to_name(err), r.http_status);
            notify_http_error(r.http_status);
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
        ESP_LOGI(TAG, "配置已重新加载: engine=%s voice=%s ds_key=%s db_token=%s",
                 s_cfg.engine,
                 s_cfg.voice,
                 s_cfg.api_key[0] ? "(已设置)" : "(空)",
                 s_cfg.db_token[0] ? "(已设置)" : "(空)");
    }
}
