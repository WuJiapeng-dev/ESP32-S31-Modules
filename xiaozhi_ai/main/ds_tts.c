/*
 * ds_tts.c - DeepSeek 语音合成客户端
 *
 * 数据通路：
 *   POST /v1/tools/tts (format=pcm)
 *     -> 响应体是 24kHz/16bit/单声道裸 PCM，边合成边返回
 *     -> 每攒够 PCM_CHUNK_SAMPLES 个样本就交给回调推 I2S
 *
 * 内存策略：读缓冲 1 KB、样本缓冲 1 KB，全程不缓存整段音频。
 * 本工程没开 PSRAM，这一点是硬约束（一段 20 秒语音就有 960 KB）。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ds_tts.h"

static const char *TAG = "tts";

#define DS_TTS_URL          "https://api.deepseek.com/v1/tools/tts"
#define HTTP_TIMEOUT_MS     20000
#define READ_BUF_SIZE       1024
#define BODY_BUF_SIZE       (DS_TEXT_MAX * 2 + 256)
#define PCM_CHUNK_SAMPLES   512         /* 攒够 512 样本（约 21ms @24kHz）推一次 */

/* 音色：官方只提供这两个 */
#define DS_VOICE_DEFAULT    "stella"

/* ------------------------------- JSON 转义 ------------------------------- */
/* 只为合法 JSON 服务：转义引号、反斜杠、控制字符；中文 UTF-8 原样保留 */
static size_t json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        char tmp[8];
        size_t tl;
        switch (*p) {
        case '"':  tmp[0] = '\\'; tmp[1] = '"';  tl = 2; break;
        case '\\': tmp[0] = '\\'; tmp[1] = '\\'; tl = 2; break;
        case '\n': tmp[0] = '\\'; tmp[1] = 'n';  tl = 2; break;
        case '\r': tmp[0] = '\\'; tmp[1] = 'r';  tl = 2; break;
        case '\t': tmp[0] = '\\'; tmp[1] = 't';  tl = 2; break;
        default:
            if (*p < 0x20) {
                tl = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            } else {
                tmp[0] = (char)*p;
                tl = 1;
            }
            break;
        }
        if (o + tl >= cap) {
            break;      /* 截断而不是溢出 */
        }
        memcpy(dst + o, tmp, tl);
        o += tl;
    }
    if (cap > 0) {
        dst[o < cap ? o : cap - 1] = '\0';
    }
    return o;
}

/* ------------------------------ 播放上下文 ------------------------------ */
typedef struct {
    int16_t  pcm[PCM_CHUNK_SAMPLES];
    size_t   pcm_n;
    uint8_t  pend_byte;     /* 块边界上落单的半个样本 */
    bool     pend_ok;
    uint32_t pcm_bytes;
    ds_pcm_cb_t cb;
    void    *user;
    bool     cb_failed;
} ctx_t;

static esp_err_t flush_pcm(ctx_t *c)
{
    if (c->pcm_n == 0) {
        return ESP_OK;
    }
    if (c->cb != NULL) {
        esp_err_t err = c->cb(c->pcm, c->pcm_n, c->user);
        if (err != ESP_OK) {
            c->cb_failed = true;
            return err;
        }
    }
    c->pcm_n = 0;
    return ESP_OK;
}

static void put_byte(ctx_t *c, uint8_t b)
{
    /* 16bit 小端；奇数位置先留着，等下一个字节补成完整样本 */
    if (!c->pend_ok) {
        c->pend_byte = b;
        c->pend_ok   = true;
        return;
    }
    c->pcm[c->pcm_n++] = (int16_t)((uint16_t)c->pend_byte | ((uint16_t)b << 8));
    c->pend_ok    = false;
    c->pcm_bytes += 2;
    if (c->pcm_n >= PCM_CHUNK_SAMPLES) {
        (void)flush_pcm(c);
    }
}

/* ------------------------------ 主流程 ------------------------------ */
esp_err_t ds_tts_speak(const app_cfg_t *cfg,
                       const char *text,
                       ds_pcm_cb_t on_pcm,
                       void *user,
                       ds_result_t *result)
{
    if (cfg == NULL || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->api_key[0] == '\0') {
        ESP_LOGE(TAG, "API Key 未配置");
        return ESP_ERR_INVALID_STATE;
    }

    size_t text_len = strlen(text);
    if (text_len > DS_TEXT_MAX) {
        ESP_LOGE(TAG, "文本 %u 字节，超过 %d 上限", (unsigned)text_len, DS_TEXT_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    char *body = malloc(BODY_BUF_SIZE);
    char *rbuf = malloc(READ_BUF_SIZE);
    char *esc  = malloc(DS_TEXT_MAX * 2);
    ctx_t *ctx = calloc(1, sizeof(ctx_t));
    if (body == NULL || rbuf == NULL || esc == NULL || ctx == NULL) {
        ESP_LOGE(TAG, "内存不足");
        free(body);
        free(rbuf);
        free(esc);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    /* ---------- 拼请求体 ---------- */
    json_escape(text, esc, DS_TEXT_MAX * 2);

    const char *voice = cfg_voice_valid(cfg->voice) ? cfg->voice : DS_VOICE_DEFAULT;

    int blen = snprintf(body, BODY_BUF_SIZE,
        "{\"text\":\"%s\",\"voice_id\":\"%s\",\"format\":\"pcm\",\"filter_markdown\":true}",
        esc, voice);

    if (blen <= 0 || blen >= BODY_BUF_SIZE) {
        ESP_LOGE(TAG, "请求体拼装溢出（%d 字节）", blen);
        free(body);
        free(rbuf);
        free(esc);
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "合成 %u 字节文本, voice=%s", (unsigned)text_len, voice);

    /* ---------- HTTP ---------- */
    char auth[CFG_KEY_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);     /* 空格分割 */

    esp_http_client_config_t hcfg = {
        .url                   = DS_TTS_URL,
        .method                = HTTP_METHOD_POST,
        .timeout_ms            = HTTP_TIMEOUT_MS,
        .buffer_size           = READ_BUF_SIZE,
        .buffer_size_tx        = BODY_BUF_SIZE,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&hcfg);
    if (cli == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        free(body);
        free(rbuf);
        free(esc);
        free(ctx);
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Content-Type", "application/json");

    ctx->cb   = on_pcm;
    ctx->user = user;

    esp_err_t ret = ESP_OK;
    int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(cli, blen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接 api.deepseek.com 失败: %s", esp_err_to_name(err));
        ret = err;
        goto cleanup;
    }

    int wlen = esp_http_client_write(cli, body, blen);
    if (wlen != blen) {
        ESP_LOGE(TAG, "请求体写入不完整: %d/%d", wlen, blen);
        ret = ESP_FAIL;
        goto cleanup;
    }

    (void)esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (result != NULL) {
        result->http_status = status;
    }
    if (status != 200) {
        /* 非 200 时响应体是 JSON 错误说明，读一段出来方便定位 */
        int n = esp_http_client_read(cli, rbuf, READ_BUF_SIZE - 1);
        if (n > 0) {
            rbuf[n] = '\0';
            ESP_LOGE(TAG, "HTTP %d，响应: %s", status, rbuf);
        } else {
            ESP_LOGE(TAG, "HTTP %d，无响应体", status);
        }
        ret = ESP_FAIL;
        goto cleanup;
    }

    /* ---------- 边收边播 ---------- */
    bool read_error = false;
    while (!ctx->cb_failed) {
        int n = esp_http_client_read(cli, rbuf, READ_BUF_SIZE);
        if (n < 0) {
            read_error = true;
            break;
        }
        if (n == 0) {
            break;      /* 收完 */
        }
        for (int i = 0; i < n; i++) {
            put_byte(ctx, (uint8_t)rbuf[i]);
        }
    }

    /* 收尾：把不满一块的样本推完 */
    if (!ctx->cb_failed) {
        (void)flush_pcm(ctx);
    }

    if (ctx->cb_failed) {
        ESP_LOGE(TAG, "音频回调中止（I2S 写失败或已停止）");
        ret = ESP_FAIL;
    } else if (read_error) {
        ESP_LOGE(TAG, "读取响应中断");
        ret = ESP_FAIL;
    } else if (ctx->pcm_bytes == 0) {
        ESP_LOGE(TAG, "HTTP 200 但没收到任何 PCM");
        ret = ESP_FAIL;
    }

    if (result != NULL) {
        result->pcm_bytes  = ctx->pcm_bytes;
        result->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }

    ESP_LOGI(TAG, "完成: %u 字节 PCM（约 %u ms 音频）, 耗时 %u ms",
             (unsigned)ctx->pcm_bytes,
             (unsigned)(ctx->pcm_bytes / 48),      /* 24000Hz * 2字节 = 48 字节/ms */
             (unsigned)((esp_timer_get_time() - t0) / 1000));

cleanup:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(body);
    free(rbuf);
    free(esc);
    free(ctx);
    return ret;
}
