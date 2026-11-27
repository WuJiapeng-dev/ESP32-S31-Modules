/*
 * llm_client.c - DeepSeek chat/completions（OpenAI 兼容格式）
 *
 * 协议依据（https://api-docs.deepseek.com/zh-cn/）：
 *   POST https://api.deepseek.com/chat/completions
 *   Authorization: Bearer <key>
 *   {"model":"deepseek-flash","messages":[{"role":"system"|"user"|"assistant","content":"..."}],
 *    "stream":false,"max_tokens":320}
 *   响应 choices[0].message.content
 */
#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_config.h"

static const char *TAG = "llm";

#define HIST_LEN 1024

/* 最近几轮的原文，环形覆盖 */
static char s_hist[DS_HISTORY_TURNS][2][HIST_LEN];
static int  s_hist_n;       /* 已存轮数 */
static int  s_hist_head;    /* 下一个写入位置 */

void llm_client_reset(void)
{
    s_hist_n    = 0;
    s_hist_head = 0;
    memset(s_hist, 0, sizeof(s_hist));
    ESP_LOGI(TAG, "对话历史已清空");
}

static void hist_push(const char *user, const char *assistant)
{
    strncpy(s_hist[s_hist_head][0], user, HIST_LEN - 1);
    s_hist[s_hist_head][0][HIST_LEN - 1] = '\0';
    strncpy(s_hist[s_hist_head][1], assistant, HIST_LEN - 1);
    s_hist[s_hist_head][1][HIST_LEN - 1] = '\0';

    s_hist_head = (s_hist_head + 1) % DS_HISTORY_TURNS;
    if (s_hist_n < DS_HISTORY_TURNS) {
        s_hist_n++;
    }
}

static int write_all(esp_http_client_handle_t c, const char *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int w = esp_http_client_write(c, data + done, (int)(len - done));
        if (w <= 0) {
            return -1;
        }
        done += (size_t)w;
    }
    return (int)done;
}

esp_err_t llm_client_chat(const app_cfg_t *cfg,
                          const char *user_text,
                          char *out_reply,
                          size_t out_size)
{
    if (cfg == NULL || user_text == NULL || out_reply == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_reply[0] = '\0';
    if (cfg->api_key[0] == '\0') {
        ESP_LOGE(TAG, "没有 API Key，请先在配网页面填写");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------- 拼请求体 ---------- */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", DS_LLM_MODEL);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", DS_LLM_MAX_TOKENS);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys  = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", DS_SYSTEM_PROMPT);
    cJSON_AddItemToArray(msgs, sys);

    /* 历史按时间正序展开：从最老的一条开始 */
    for (int i = 0; i < s_hist_n; i++) {
        int idx = (s_hist_head - s_hist_n + i + DS_HISTORY_TURNS * 2) % DS_HISTORY_TURNS;
        for (int k = 0; k < 2; k++) {
            if (s_hist[idx][k][0] == '\0') {
                continue;
            }
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", (k == 0) ? "user" : "assistant");
            cJSON_AddStringToObject(m, "content", s_hist[idx][k]);
            cJSON_AddItemToArray(msgs, m);
        }
    }

    cJSON *cur = cJSON_CreateObject();
    cJSON_AddStringToObject(cur, "role", "user");
    cJSON_AddStringToObject(cur, "content", user_text);
    cJSON_AddItemToArray(msgs, cur);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);

    esp_http_client_config_t hc = {
        .url               = DS_CHAT_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 60000,
        .buffer_size       = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (c == NULL) {
        free(body);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;

    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");

    size_t body_len = strlen(body);
    if (esp_http_client_open(c, (int)body_len) != ESP_OK) {
        ESP_LOGE(TAG, "建立连接失败（检查 Wi-Fi 与 API Key）");
        goto out;
    }
    if (write_all(c, body, body_len) < 0) {
        ESP_LOGE(TAG, "发送请求体失败");
        goto out;
    }
    free(body);
    body = NULL;

    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGE(TAG, "读取响应头失败");
        goto out;
    }

    {
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGE(TAG, "LLM 返回 HTTP %d（401=Key 无效 402=余额不足 "
                          "429=限流 503=服务繁忙）", status);
            goto out;
        }
    }

    /* 响应可能 1~2KB，读到动态缓冲里 */
    static char resp[4096];
    int n = 0;
    while (n < (int)sizeof(resp) - 1) {
        int r = esp_http_client_read(c, resp + n, (int)sizeof(resp) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += r;
    }
    resp[n] = '\0';
    if (n == 0) {
        ESP_LOGE(TAG, "LLM 响应为空");
        goto out;
    }

    {
        cJSON *jr = cJSON_Parse(resp);
        if (jr == NULL) {
            ESP_LOGE(TAG, "LLM 响应不是合法 JSON：%.120s", resp);
            goto out;
        }
        cJSON *choices = cJSON_GetObjectItem(jr, "choices");
        cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg     = first ? cJSON_GetObjectItem(first, "message") : NULL;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;

        if (cJSON_IsString(content) && content->valuestring != NULL) {
            strncpy(out_reply, content->valuestring, out_size - 1);
            out_reply[out_size - 1] = '\0';
            err = ESP_OK;
            hist_push(user_text, out_reply);
        } else {
            ESP_LOGE(TAG, "LLM 响应里没有 choices[0].message.content");
        }
        cJSON_Delete(jr);
    }

out:
    if (body != NULL) {
        free(body);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}
