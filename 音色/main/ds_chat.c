#include "ds_chat.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "ds_chat"

#define DS_CHAT_URL      "https://api.deepseek.com/chat/completions"
#define DS_CHAT_MODEL    "deepseek-chat"
#define CHAT_TIMEOUT_MS  25000
#define REQ_BUF_SIZE     4096
#define RSP_BUF_SIZE     6144
#define ESC_BUF_SIZE     2048
#define MAX_TOKENS       800

/* ---------------- JSON 转义 / 反转义 ---------------- */

int ds_chat_json_escape(const char *src, char *dst, int cap)
{
    if (src == NULL || dst == NULL || cap <= 1) {
        return -1;
    }

    int o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != 0; p++) {
        char tmp[16];
        int n;

        switch (*p) {
        case '"':  tmp[0] = '\\'; tmp[1] = '"';  n = 2; break;
        case '\\': tmp[0] = '\\'; tmp[1] = '\\'; n = 2; break;
        case '\n': tmp[0] = '\\'; tmp[1] = 'n';  n = 2; break;
        case '\r': tmp[0] = '\\'; tmp[1] = 'r';  n = 2; break;
        case '\t': tmp[0] = '\\'; tmp[1] = 't';  n = 2; break;
        default:
            if (*p < 0x20) {
                n = snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
                if (n < 0) {
                    return -1;
                }
            } else {
                tmp[0] = (char)*p;
                n = 1;
            }
            break;
        }

        if (n <= 0 || o + n >= cap) {
            dst[o] = '\0';
            return -1;
        }
        memcpy(dst + o, tmp, (size_t)n);
        o += n;
    }

    dst[o] = '\0';
    return o;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

/* 把 JSON 字符串内容反转义进 dst，长度由 slen 限定；总是以 '\0' 结尾 */
static void json_unescape(const char *src, int slen, char *dst, int cap)
{
    int o = 0;
    int i = 0;

    while (i < slen && o < cap - 1) {
        char c = src[i++];

        if (c != '\\') {
            dst[o++] = c;
            continue;
        }
        if (i >= slen) {
            break;
        }

        char e = src[i++];
        switch (e) {
        case '"':  dst[o++] = '"';  break;
        case '\\': dst[o++] = '\\'; break;
        case '/':  dst[o++] = '/';  break;
        case 'b':  dst[o++] = '\b'; break;
        case 'f':  dst[o++] = '\f'; break;
        case 'n':  dst[o++] = '\n'; break;
        case 'r':  dst[o++] = '\r'; break;
        case 't':  dst[o++] = '\t'; break;
        case 'u': {
            if (i + 4 > slen) {
                i = slen;
                break;
            }
            int cp = hex4(src + i);
            i += 4;
            if (cp < 0) {
                break;
            }
            /* 代理对：D800-DBFF 后面跟 DC00-DFFF */
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= slen &&
                src[i] == '\\' && src[i + 1] == 'u') {
                int lo = hex4(src + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp < 0x80) {
                if (o + 1 < cap) {
                    dst[o++] = (char)cp;
                }
            } else if (cp < 0x800) {
                if (o + 2 < cap) {
                    dst[o++] = (char)(0xC0 | (cp >> 6));
                    dst[o++] = (char)(0x80 | (cp & 0x3F));
                }
            } else if (cp < 0x10000) {
                if (o + 3 < cap) {
                    dst[o++] = (char)(0xE0 | (cp >> 12));
                    dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (cp & 0x3F));
                }
            } else {
                if (o + 4 < cap) {
                    dst[o++] = (char)(0xF0 | (cp >> 18));
                    dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (cp & 0x3F));
                }
            }
            break;
        }
        default:
            dst[o++] = e;
            break;
        }
    }

    dst[o] = '\0';
}

/* ---------------- HTTP ---------------- */

typedef struct {
    char *buf;
    int   cap;
    int   len;
} rsp_ctx_t;

static esp_err_t http_ev(esp_http_client_event_t *evt)
{
    rsp_ctx_t *c = (rsp_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && c != NULL && evt->data_len > 0) {
        int room = c->cap - 1 - c->len;
        int n = (evt->data_len < room) ? evt->data_len : room;
        if (n > 0) {
            memcpy(c->buf + c->len, evt->data, (size_t)n);
            c->len += n;
            c->buf[c->len] = '\0';
        }
    }
    return ESP_OK;
}

/* 在响应里找 choices[0].message.content 的原文区间 */
static bool pick_content(const char *json, const char **start, int *len)
{
    const char *p = strstr(json, "\"choices\"");
    if (p == NULL) {
        return false;
    }

    p = strstr(p, "\"content\"");
    if (p == NULL) {
        return false;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;       /* content 为 null（思维链模型可能如此） */
    }
    p++;

    const char *q = p;
    while (*q != 0) {
        if (*q == '\\' && q[1] != 0) {
            q += 2;
            continue;
        }
        if (*q == '"') {
            *start = p;
            *len = (int)(q - p);
            return true;
        }
        q++;
    }
    return false;
}

esp_err_t ds_chat_ask(const char *api_key, const char *question,
                      char *reply, size_t reply_size, int *http_status)
{
    if (http_status != NULL) {
        *http_status = 0;
    }
    if (api_key == NULL || api_key[0] == '\0' || question == NULL ||
        reply == NULL || reply_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    reply[0] = '\0';

    char *esc = malloc(ESC_BUF_SIZE);
    char *req = malloc(REQ_BUF_SIZE);
    char *rsp = malloc(RSP_BUF_SIZE);
    if (esc == NULL || req == NULL || rsp == NULL) {
        free(esc);
        free(req);
        free(rsp);
        return ESP_ERR_NO_MEM;
    }

    int elen = ds_chat_json_escape(question, esc, ESC_BUF_SIZE);
    if (elen < 0) {
        free(esc);
        free(req);
        free(rsp);
        return ESP_ERR_INVALID_SIZE;        /* 问题太长 */
    }

    int rlen = snprintf(req, REQ_BUF_SIZE,
                        "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                        "\"content\":\"%s\"}],\"stream\":false,"
                        "\"max_tokens\":%d}",
                        DS_CHAT_MODEL, esc, MAX_TOKENS);
    free(esc);

    if (rlen <= 0 || rlen >= REQ_BUF_SIZE) {
        free(req);
        free(rsp);
        return ESP_ERR_INVALID_SIZE;
    }

    rsp[0] = '\0';
    rsp_ctx_t ctx = { .buf = rsp, .cap = RSP_BUF_SIZE, .len = 0 };

    /* api_key 最长 127 字节（CFG_KEY_MAX-1），“Bearer ” 占 7 字节，再留 1 字节 '\0'。
       缓冲给小了会静默截断，DeepSeek 返回 401，看起来像“Key 填错了”，极难排查。 */
    char auth[160];
    int alen = snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    if (alen <= 0 || alen >= (int)sizeof(auth)) {
        free(req);
        free(rsp);
        ESP_LOGE(TAG, "API Key 过长（%d 字节）", (int)strlen(api_key));
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = {
        .url                   = DS_CHAT_URL,
        .method                = HTTP_METHOD_POST,
        .timeout_ms            = CHAT_TIMEOUT_MS,
        .buffer_size           = 1024,
        .buffer_size_tx        = 1024,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler         = http_ev,
        .user_data             = &ctx,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        free(req);
        free(rsp);
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, req, rlen);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    if (http_status != NULL) {
        *http_status = status;
    }
    esp_http_client_cleanup(cli);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "请求失败: %s", esp_err_to_name(err));
        free(req);
        free(rsp);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d，响应: %.200s", status, rsp);
        free(req);
        free(rsp);
        return ESP_FAIL;
    }

    const char *cs = NULL;
    int clen = 0;
    if (!pick_content(rsp, &cs, &clen)) {
        ESP_LOGE(TAG, "响应里没有 content: %.200s", rsp);
        free(req);
        free(rsp);
        return ESP_ERR_NOT_FOUND;
    }

    json_unescape(cs, clen, reply, (int)reply_size);
    ESP_LOGI(TAG, "回答 %d 字节", (int)strlen(reply));

    free(req);
    free(rsp);
    return ESP_OK;
}
