/*
 * asr_client.c - DeepSeek ASR 的 multipart/form-data 上传实现
 *
 * 协议依据（https://api-docs.deepseek.com/zh-cn/guides/asr/）：
 *   POST https://api.deepseek.com/v1/tools/asr
 *   Authorization: Bearer <key>
 *   Content-Type: multipart/form-data; boundary=...
 *   file 字段放音频；WAV 必须是 16bit PCM，采样率 8000~48000，单声道或双声道（只识别左声道）
 *   成功返回 {"request_id":"...","text":"...","duration_ms":3200}
 *
 * 为什么要自己拼 multipart：esp_http_client 没有表单上传的封装，
 * 而 PCM 数据量大（8 秒 256KB），必须先 open 拿到句柄再分块 write，
 * 否则拼成一整个 buffer 会吃掉两块 256KB 内存。
 */
#include "asr_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "mic_i2s.h"        /* MIC_SAMPLE_RATE */

static const char *TAG = "asr";

#define ASR_BOUNDARY    "----esp32asr7f3a"
#define ASR_WRITE_CHUNK 4096

/* ---------- 小端写入 WAV 头 ---------- */
static void wav_header(uint8_t h[44], uint32_t data_bytes, uint32_t sample_rate, uint16_t channels)
{
    uint32_t byte_rate   = sample_rate * channels * 2;
    uint16_t block_align = (uint16_t)(channels * 2);

    memcpy(h + 0, "RIFF", 4);
    h[4]  = (uint8_t)(data_bytes + 36);
    h[5]  = (uint8_t)((data_bytes + 36) >> 8);
    h[6]  = (uint8_t)((data_bytes + 36) >> 16);
    h[7]  = (uint8_t)((data_bytes + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;  h[17] = 0;             /* fmt chunk size */
    h[18] = 1;   h[19] = 0;             /* PCM */
    h[20] = (uint8_t)channels; h[21] = 0;
    h[22] = (uint8_t)sample_rate;
    h[23] = (uint8_t)(sample_rate >> 8);
    h[24] = (uint8_t)(sample_rate >> 16);
    h[25] = (uint8_t)(sample_rate >> 24);
    h[26] = (uint8_t)byte_rate;
    h[27] = (uint8_t)(byte_rate >> 8);
    h[28] = (uint8_t)(byte_rate >> 16);
    h[29] = (uint8_t)(byte_rate >> 24);
    h[30] = (uint8_t)block_align; h[31] = (uint8_t)(block_align >> 8);
    h[32] = 16;  h[33] = 0;             /* bits per sample */
    memcpy(h + 34, "data", 4);
    h[38] = (uint8_t)data_bytes;
    h[39] = (uint8_t)(data_bytes >> 8);
    h[40] = (uint8_t)(data_bytes >> 16);
    h[41] = (uint8_t)(data_bytes >> 24);
    h[42] = 0; h[43] = 0;
}

/* 分块写满，返回写入总字节数；失败返回 -1 */
static int write_all(esp_http_client_handle_t c, const uint8_t *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > ASR_WRITE_CHUNK) {
            n = ASR_WRITE_CHUNK;
        }
        int w = esp_http_client_write(c, (const char *)data + done, (int)n);
        if (w <= 0) {
            return -1;
        }
        done += (size_t)w;
    }
    return (int)done;
}

esp_err_t asr_client_transcribe(const app_cfg_t *cfg,
                                const int16_t *pcm,
                                size_t samples,
                                char *out_text,
                                size_t out_size)
{
    if (cfg == NULL || pcm == NULL || samples == 0 || out_text == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    uint32_t data_bytes = (uint32_t)(samples * 2);

    char pre[192];
    int pre_len = snprintf(pre, sizeof(pre),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           ASR_BOUNDARY);

    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", ASR_BOUNDARY);

    size_t total = (size_t)pre_len + 44 + data_bytes + (size_t)tail_len;

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);

    esp_http_client_config_t hc = {
        .url               = DS_ASR_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 40000,
        .buffer_size       = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (c == NULL) {
        ESP_LOGE(TAG, "创建 HTTP 客户端失败");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    char hdr_content_type[96];
    snprintf(hdr_content_type, sizeof(hdr_content_type),
             "multipart/form-data; boundary=%s", ASR_BOUNDARY);

    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", hdr_content_type);

    ESP_LOGI(TAG, "上传音频 %u 字节（%.1f 秒）…", (unsigned)total,
             (double)samples / (double)MIC_SAMPLE_RATE);

    if (esp_http_client_open(c, (int)total) != ESP_OK) {
        ESP_LOGE(TAG, "建立连接失败（检查 Wi-Fi 与 API Key）");
        goto out;
    }

    if (write_all(c, (const uint8_t *)pre, (size_t)pre_len) < 0) {
        ESP_LOGE(TAG, "写入表单头失败");
        goto out;
    }

    {
        uint8_t wav[44];
        wav_header(wav, data_bytes, MIC_SAMPLE_RATE, 1);
        if (write_all(c, wav, sizeof(wav)) < 0) {
            ESP_LOGE(TAG, "写入 WAV 头失败");
            goto out;
        }
    }

    if (write_all(c, (const uint8_t *)pcm, data_bytes) < 0) {
        ESP_LOGE(TAG, "写入音频数据失败");
        goto out;
    }

    if (write_all(c, (const uint8_t *)tail, (size_t)tail_len) < 0) {
        ESP_LOGE(TAG, "写入表单尾失败");
        goto out;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGE(TAG, "读取响应头失败");
        goto out;
    }

    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGE(TAG, "ASR 返回 HTTP %d（400=音频格式不对 401=Key 无效 "
                      "429=并发超限 503=服务繁忙）", status);
        goto out;
    }

    /* 响应很小，一次读完；cJSON 需要以 '\0' 结尾 */
    static char resp[2048];
    int total_read = 0;
    while (total_read < (int)sizeof(resp) - 1) {
        int r = esp_http_client_read(c, resp + total_read, (int)sizeof(resp) - 1 - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
    }
    resp[total_read] = '\0';

    if (total_read == 0) {
        ESP_LOGE(TAG, "ASR 响应为空");
        goto out;
    }

    {
        cJSON *root = cJSON_Parse(resp);
        if (root == NULL) {
            ESP_LOGE(TAG, "ASR 响应不是合法 JSON：%.120s", resp);
            goto out;
        }
        cJSON *t = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(t) && t->valuestring != NULL) {
            strncpy(out_text, t->valuestring, out_size - 1);
            out_text[out_size - 1] = '\0';
            err = ESP_OK;
        } else {
            ESP_LOGE(TAG, "ASR 响应里没有 text 字段");
        }
        cJSON_Delete(root);
    }

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}
