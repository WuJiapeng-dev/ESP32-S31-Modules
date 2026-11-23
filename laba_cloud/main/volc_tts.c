/*
 * volc_tts.c - 火山引擎 TTS HTTP 客户端（流式解析 + 增量 base64 解码）
 *
 * 为什么不用 cJSON：
 *   响应体里 data 字段是整段音频的 base64，长度可达数百 KB。用 cJSON 解析意味着
 *   先把整个响应体读进内存再 parse，而本工程没开 PSRAM，SRAM 只有几百 KB、
 *   WiFi + TLS 还要占掉一大半，一定会 OOM。这里改成状态机流式扫描，内存恒定几 KB。
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "audio_out.h"
#include "volc_tts.h"

static const char *TAG = "tts";

#define VOLC_TTS_URL        "https://openspeech.bytedance.com/api/v1/tts"
#define VOLC_TEXT_MAX       1024        /* 官方：text 上限 1024 UTF-8 字节 */
#define HTTP_TIMEOUT_MS     20000
#define READ_BUF_SIZE       1024
#define BODY_BUF_SIZE       2048
#define PCM_CHUNK_SAMPLES   512         /* 攒够 512 样本就推一次 I2S */

/* 调试用：进入 data 段之前把原始字节留一份，出错时打印 */
#define HEAD_KEEP           512

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

/* ---------------------------- base64 增量解码 ---------------------------- */
typedef struct {
    uint8_t q[4];
    int     n;
} b64_t;

#define B64_PAD 0xFE

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 喂一个字符，解出 0~3 字节到 out，返回解出字节数 */
static int b64_feed(b64_t *d, char c, uint8_t out[3])
{
    int v;
    if (c == '=') {
        v = B64_PAD;
    } else {
        v = b64_val(c);
        if (v < 0) {
            return 0;       /* 非 base64 字符（理论上不会出现），忽略 */
        }
    }

    d->q[d->n++] = (uint8_t)v;
    if (d->n < 4) {
        return 0;
    }
    d->n = 0;

    uint32_t x = 0;
    int pad = 0;
    for (int i = 0; i < 4; i++) {
        x <<= 6;
        if (d->q[i] == B64_PAD) {
            pad++;
        } else {
            x |= d->q[i];
        }
    }

    out[0] = (uint8_t)((x >> 16) & 0xFF);
    out[1] = (uint8_t)((x >> 8) & 0xFF);
    out[2] = (uint8_t)(x & 0xFF);
    return 3 - pad;
}

/* ------------------------------ 解析状态机 ------------------------------ */
typedef enum {
    ST_SEEK_CODE = 0,
    ST_READ_CODE,
    ST_SKIP_TO_DATA,        /* code 非 3000 时，继续收一点好打印错因 */
    ST_SEEK_DATA,
    ST_DATA,
    ST_DONE,
} pstate_t;

typedef struct {
    pstate_t st;
    char     win[16];       /* 滚动窗口，用来跨 chunk 匹配关键字 */
    int      winlen;
    int      code;
    b64_t    b64;
    int16_t  pcm[PCM_CHUNK_SAMPLES];
    size_t   pcm_n;
    uint8_t  pend_byte;     /* 孤立的高/低字节，等下一个补成完整样本 */
    bool     pend_ok;
    volc_pcm_cb_t cb;
    void    *user;
    uint32_t pcm_bytes;
    char     head[HEAD_KEEP];
    size_t   head_len;
    bool     cb_failed;
} ctx_t;

/* 窗口末尾是否正好是 pat */
static bool win_endswith(const char *win, int winlen, const char *pat)
{
    int plen = (int)strlen(pat);
    if (winlen < plen) {
        return false;
    }
    return memcmp(win + winlen - plen, pat, (size_t)plen) == 0;
}

static void win_push(ctx_t *c, char ch)
{
    if (c->winlen >= (int)sizeof(c->win)) {
        memmove(c->win, c->win + 1, sizeof(c->win) - 1);
        c->winlen = (int)sizeof(c->win) - 1;
    }
    c->win[c->winlen++] = ch;
}

/* 把攒够的 PCM 推出去 */
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

static void put_pcm_byte(ctx_t *c, uint8_t b)
{
    /* 16bit 样本按小端拼；奇数字节先留着，下个字节补上 */
    if (!c->pend_ok) {
        c->pend_byte = b;
        c->pend_ok   = true;
        return;
    }
    int16_t s = (int16_t)((uint16_t)c->pend_byte | ((uint16_t)b << 8));
    c->pend_ok = false;

    c->pcm[c->pcm_n++] = s;
    c->pcm_bytes += 2;
    if (c->pcm_n >= PCM_CHUNK_SAMPLES) {
        (void)flush_pcm(c);
    }
}

static esp_err_t feed_byte(ctx_t *c, char ch)
{
    switch (c->st) {
    case ST_SEEK_CODE:
        win_push(c, ch);
        if (win_endswith(c->win, c->winlen, "\"code\":")) {
            c->st = ST_READ_CODE;
            c->winlen = 0;
        }
        break;

    case ST_READ_CODE:
        if (c->winlen == 0 && (ch == ' ' || ch == '\t')) {
            break;      /* 冒号后的空白 */
        }
        if (ch >= '0' && ch <= '9') {
            c->code = c->code * 10 + (ch - '0');
            break;
        }
        /* 数字结束 */
        c->winlen = 0;
        if (c->code == 3000) {
            c->st = ST_SEEK_DATA;
            win_push(c, ch);    /* 这个字符可能是 "data" 的开头 */
        } else {
            ESP_LOGE(TAG, "火山返回业务码 %d", c->code);
            c->st = ST_SKIP_TO_DATA;    /* 继续读，让调用方打印原文 */
        }
        break;

    case ST_SKIP_TO_DATA:
        /* 什么也不做，靠 head 缓冲留证据，读完整段后统一报错 */
        break;

    case ST_SEEK_DATA:
        win_push(c, ch);
        if (win_endswith(c->win, c->winlen, "\"data\":\"")) {
            c->st = ST_DATA;
            c->winlen = 0;
        }
        break;

    case ST_DATA: {
        if (ch == '"') {           /* base64 字符集里没有引号，这就是结束符 */
            c->st = ST_DONE;
            break;
        }
        uint8_t out[3];
        int n = b64_feed(&c->b64, ch, out);
        for (int i = 0; i < n; i++) {
            put_pcm_byte(c, out[i]);
        }
        break;
    }

    case ST_DONE:
    default:
        break;
    }
    return ESP_OK;
}

/* ------------------------------ 主流程 ------------------------------ */
esp_err_t volc_tts_speak(const app_cfg_t *cfg,
                         const char *text,
                         volc_pcm_cb_t on_pcm,
                         void *user,
                         volc_result_t *result)
{
    if (cfg == NULL || text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t text_len = strlen(text);
    if (text_len > VOLC_TEXT_MAX) {
        ESP_LOGE(TAG, "文本 %u 字节，超过火山 V1 上限 %d", (unsigned)text_len, VOLC_TEXT_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }

    char *body = malloc(BODY_BUF_SIZE);
    char *rbuf = malloc(READ_BUF_SIZE);
    char *esc  = malloc(VOLC_TEXT_MAX * 2);
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
    json_escape(text, esc, VOLC_TEXT_MAX * 2);

    char reqid[33];
    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    snprintf(reqid, sizeof(reqid), "%08x%08x%08x%08x", (unsigned)r0, (unsigned)r1,
             (unsigned)r2, (unsigned)r3);

    int blen = snprintf(body, BODY_BUF_SIZE,
        "{\"app\":{\"appid\":\"%s\",\"token\":\"%s\",\"cluster\":\"%s\"},"
        "\"user\":{\"uid\":\"esp32s31\"},"
        "\"audio\":{\"voice_type\":\"%s\",\"encoding\":\"pcm\",\"rate\":%d,"
        "\"speed_ratio\":%.1f,\"volume_ratio\":%.1f},"
        "\"request\":{\"reqid\":\"%s\",\"text\":\"%s\",\"text_type\":\"plain\",\"operation\":\"query\"}}",
        cfg->appid, cfg->token,
        cfg->cluster[0] ? cfg->cluster : CFG_CLUSTER_DEFAULT,
        cfg->voice,
        AUDIO_SAMPLE_RATE,
        (double)cfg->speed_x100 / 100.0,
        (double)cfg->volume_x100 / 100.0,
        reqid, esc);

    if (blen <= 0 || blen >= BODY_BUF_SIZE) {
        ESP_LOGE(TAG, "请求体拼装溢出（%d 字节）", blen);
        free(body);
        free(rbuf);
        free(esc);
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "合成 %u 字节文本, voice=%s, speed=%.1f, volume=%.1f",
             (unsigned)text_len, cfg->voice,
             (double)cfg->speed_x100 / 100.0, (double)cfg->volume_x100 / 100.0);

    /* ---------- HTTP ---------- */
    char auth[CFG_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer; %s", cfg->token);   /* 分号！ */

    esp_http_client_config_t hcfg = {
        .url                = VOLC_TTS_URL,
        .method             = HTTP_METHOD_POST,
        .timeout_ms         = HTTP_TIMEOUT_MS,
        .buffer_size        = READ_BUF_SIZE,
        .buffer_size_tx     = BODY_BUF_SIZE,
        .crt_bundle_attach  = esp_crt_bundle_attach,
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

    ctx->cb      = on_pcm;
    ctx->user    = user;
    ctx->st      = ST_SEEK_CODE;
    ctx->code    = 0;
    ctx->head_len = 0;

    esp_err_t ret = ESP_OK;
    int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(cli, blen);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接 openspeech.bytedance.com 失败: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "HTTP 状态码 %d", status);
        /* 把响应体读一小段出来方便定位（通常里面带错误说明） */
        int n = esp_http_client_read(cli, rbuf, READ_BUF_SIZE - 1);
        if (n > 0) {
            rbuf[n] = '\0';
            ESP_LOGE(TAG, "响应: %s", rbuf);
        }
        ret = ESP_FAIL;
        goto cleanup;
    }

    /* ---------- 流式读取 + 解析 ---------- */
    bool read_error = false;
    while (true) {
        int n = esp_http_client_read(cli, rbuf, READ_BUF_SIZE);
        if (n < 0) {
            read_error = true;
            break;
        }
        if (n == 0) {
            break;      /* 收完 */
        }
        for (int i = 0; i < n; i++) {
            char ch = rbuf[i];

            /* 进入 data 段之前留一份原文，出错好查 */
            if (ctx->st != ST_DATA && ctx->head_len < HEAD_KEEP - 1) {
                ctx->head[ctx->head_len++] = ch;
                ctx->head[ctx->head_len] = '\0';
            }

            feed_byte(ctx, ch);
            if (ctx->cb_failed) {
                break;
            }
        }
        if (ctx->st == ST_DONE || ctx->cb_failed) {
            break;
        }
    }

    /* 收尾：把一个块里剩下的样本推完 */
    if (!ctx->cb_failed) {
        (void)flush_pcm(ctx);
    }

    if (ctx->cb_failed) {
        ESP_LOGE(TAG, "音频回调中止（I2S 写失败或已停止）");
        ret = ESP_FAIL;
    } else if (ctx->code != 3000) {
        ESP_LOGE(TAG, "合成失败，业务码 %d，响应前缀: %.300s", ctx->code,
                 ctx->head_len ? ctx->head : "(空)");
        ret = ESP_FAIL;
    } else if (read_error) {
        ESP_LOGE(TAG, "读取响应中断");
        ret = ESP_FAIL;
    } else if (ctx->pcm_bytes == 0) {
        ESP_LOGE(TAG, "业务码 3000 但没解出任何 PCM");
        ret = ESP_FAIL;
    }

    if (result != NULL) {
        result->api_code  = ctx->code;
        result->pcm_bytes = ctx->pcm_bytes;
        result->elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }

    ESP_LOGI(TAG, "完成: %u 字节 PCM, %u ms, code=%d",
             (unsigned)ctx->pcm_bytes,
             (unsigned)((esp_timer_get_time() - t0) / 1000), ctx->code);

cleanup:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(body);
    free(rbuf);
    free(esc);
    free(ctx);
    return ret;
}
