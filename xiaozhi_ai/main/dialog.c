/*
 * dialog.c - 语音对话主循环
 *
 * 数据流：
 *   INMP441 --I2S(16k/32bit槽/右声道)--> 每 30ms 一帧
 *     -> 能量 VAD（噪声底 0.95/0.05 一阶平滑，阈值 max(3×底, 300)）
 *     -> 触发后把 200ms 预滚 + 后续语音写进 PSRAM 录音缓冲
 *     -> 静音 700ms 或满 8 秒收尾
 *     -> asr_client_transcribe()  DeepSeek /v1/tools/asr
 *     -> llm_client_chat()        DeepSeek /chat/completions
 *     -> ds_tts_speak()           DeepSeek /v1/tools/tts（format=pcm, 24kHz）
 *     -> audio_out_write_mono16() MAX98357 -> 喇叭
 *
 * 为什么播报期间不录音：MAX98357 出来的声音会被 INMP441 直接收进去，
 * 形成自激循环（回答念到一半又触发识别）。本工程不做回声消除，
 * 用「播放时暂停采集」这个最简单可靠的办法。
 */
#include "dialog.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "asr_client.h"
#include "audio_out.h"
#include "cfg_store.h"
#include "ds_tts.h"
#include "llm_client.h"
#include "mic_i2s.h"
#include "wifi_prov.h"

static const char *TAG = "dialog";

#define FRAME_MS            30
#define FRAME_SAMPLES       (MIC_SAMPLE_RATE * FRAME_MS / 1000)          /* 480 */
#define PREROLL_FRAMES      (DIALOG_PREROLL_MS / FRAME_MS)               /* 6 */
#define MAX_RECORD_SAMPLES  (MIC_SAMPLE_RATE * DIALOG_MAX_RECORD_MS / 1000)

/* 触发门限的绝对下限：安静环境下噪声底可能只有 30~80，乘 3 也不够 */
#define VAD_THR_MIN         300.0f
/* 等待说话的轮询上限，用来定期回到主循环检查 Wi-Fi / 配置是否还在 */
#define IDLE_FRAMES_MAX     2000          /* 2000 × 30ms = 60 秒 */

static int16_t *s_rec  = NULL;            /* PSRAM 录音缓冲 */
static int16_t  s_pre[PREROLL_FRAMES][FRAME_SAMPLES];
static int      s_pre_n = 0;
static volatile bool s_speaking = false;

static float frame_rms(const int16_t *p, int n)
{
    if (n <= 0) {
        return 0.0f;
    }
    float acc = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = (float)p[i];
        acc += v * v;
    }
    return sqrtf(acc / (float)n);
}

static void pre_push(const int16_t *frame)
{
    if (s_pre_n < PREROLL_FRAMES) {
        memcpy(s_pre[s_pre_n], frame, FRAME_SAMPLES * sizeof(int16_t));
        s_pre_n++;
    } else {
        memmove(s_pre[0], s_pre[1], (PREROLL_FRAMES - 1) * FRAME_SAMPLES * sizeof(int16_t));
        memcpy(s_pre[PREROLL_FRAMES - 1], frame, FRAME_SAMPLES * sizeof(int16_t));
    }
}

/*
 * 阻塞等到一句话录完，返回样本数（0 = 放弃/超时）。
 */
static size_t record_one_utterance(void)
{
    static int16_t frame[FRAME_SAMPLES];

    float floor_rms = 200.0f;
    int   hit = 0;
    int   sil = 0;
    int   idle = 0;
    size_t n = 0;
    bool  rec = false;
    float thr = VAD_THR_MIN;

    s_pre_n = 0;

    for (;;) {
        size_t got = mic_i2s_read(frame, FRAME_SAMPLES, 200);
        if (got == 0) {
            vTaskDelay(1);
            continue;
        }
        if (got < FRAME_SAMPLES) {
            memset(frame + got, 0, (FRAME_SAMPLES - got) * sizeof(int16_t));
        }

        float rms = frame_rms(frame, FRAME_SAMPLES);

        if (!rec) {
            /* 噪声底：只在没说话时跟踪 */
            if (rms < floor_rms * 1.5f) {
                floor_rms = 0.95f * floor_rms + 0.05f * rms;
            }
            if (floor_rms < 30.0f) {
                floor_rms = 30.0f;
            }
            thr = floor_rms * 3.0f;
            if (thr < VAD_THR_MIN) {
                thr = VAD_THR_MIN;
            }

            pre_push(frame);

            if (rms > thr) {
                hit++;
            } else {
                hit = 0;
            }

            if (hit >= 3) {
                /* 连续 90ms 过阈，认为开口了：把预滚搬进录音缓冲 */
                rec = true;
                sil = 0;
                n = 0;
                for (int i = 0; i < s_pre_n && n + FRAME_SAMPLES <= MAX_RECORD_SAMPLES; i++) {
                    memcpy(s_rec + n, s_pre[i], FRAME_SAMPLES * sizeof(int16_t));
                    n += FRAME_SAMPLES;
                }
                s_pre_n = 0;
                ESP_LOGI(TAG, "检测到说话，开始录音（噪声底 %.0f，阈值 %.0f）",
                         (double)floor_rms, (double)thr);
            } else if (++idle > IDLE_FRAMES_MAX) {
                return 0;
            }
            continue;
        }

        /* 录音中 */
        if (n + got <= MAX_RECORD_SAMPLES) {
            memcpy(s_rec + n, frame, got * sizeof(int16_t));
            n += got;
        }

        if (rms < thr) {
            sil++;
        } else {
            sil = 0;
        }

        if ((uint32_t)(sil * FRAME_MS) >= DIALOG_SILENCE_MS ||
            n >= MAX_RECORD_SAMPLES) {
            break;
        }
    }

    if ((uint32_t)(n * 1000 / MIC_SAMPLE_RATE) < DIALOG_MIN_VOICE_MS) {
        ESP_LOGI(TAG, "语音太短（%u 样本），丢弃", (unsigned)n);
        return 0;
    }
    return n;
}

/* TTS 边收边播的回调 */
static esp_err_t on_tts_pcm(const int16_t *pcm, size_t samples, void *user)
{
    (void)user;
    return audio_out_write_mono16(pcm, samples);
}

static void run_one_dialog(void)
{
    app_cfg_t cfg;
    bool ready = (cfg_load(&cfg) == ESP_OK) && cfg_ready(&cfg);

    if (!wifi_sta_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    if (!ready) {
        static uint32_t last;
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last > 15000) {
            last = now;
            ESP_LOGW(TAG, "配置不完整，请连热点 %s 打开 http://%s/ 填写 Wi-Fi 与 API Key",
                     PROV_AP_SSID, PROV_AP_IP);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }

    size_t n = record_one_utterance();
    if (n == 0) {
        return;
    }

    uint32_t ms = (uint32_t)(n * 1000ULL / MIC_SAMPLE_RATE);
    ESP_LOGI(TAG, "录音结束：%u 样本 / %u ms，开始识别…", (unsigned)n, (unsigned)ms);

    /* ---------- 1. ASR ---------- */
    static char text[ASR_TEXT_MAX];
    if (asr_client_transcribe(&cfg, s_rec, n, text, sizeof(text)) != ESP_OK) {
        ESP_LOGE(TAG, "识别失败，本轮结束（串口上看上面一行错误码）");
        return;
    }
    if (text[0] == '\0') {
        ESP_LOGW(TAG, "识别结果为空，本轮结束");
        return;
    }
    ESP_LOGI(TAG, "【识别】%s", text);

    /* ---------- 2. LLM ---------- */
    static char reply[LLM_REPLY_MAX];
    if (llm_client_chat(&cfg, text, reply, sizeof(reply)) != ESP_OK) {
        ESP_LOGE(TAG, "对话失败，本轮结束");
        return;
    }
    ESP_LOGI(TAG, "【AI】%s", reply);

    /* ---------- 3. TTS 播报 ---------- */
    s_speaking = true;
    audio_out_set_enable(true);

    ds_result_t res = {0};
    esp_err_t err = ds_tts_speak(&cfg, reply, on_tts_pcm, NULL, &res);

    audio_out_drain();
    audio_out_set_enable(false);
    s_speaking = false;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "播报完成：%u 字节 PCM，耗时 %u ms，HTTP %d",
                 (unsigned)res.pcm_bytes, (unsigned)res.elapsed_ms, res.http_status);
    } else {
        ESP_LOGE(TAG, "播报失败（HTTP %d），检查 API Key 余额与网络", res.http_status);
    }
}

static void dialog_task(void *arg)
{
    (void)arg;

    s_rec = (int16_t *)heap_caps_malloc(DIALOG_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rec == NULL) {
        ESP_LOGW(TAG, "PSRAM 分配失败，改用内部 RAM（录音上限降到 3 秒）");
        s_rec = (int16_t *)heap_caps_malloc(MIC_SAMPLE_RATE * 2 * 3, MALLOC_CAP_8BIT);
    }
    if (s_rec == NULL) {
        ESP_LOGE(TAG, "录音缓冲分配失败，对话任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "对话任务就绪：说话即识别，无需按键");
    ESP_LOGI(TAG, "单句上限 %d 秒，静音 %d ms 判结束",
             DIALOG_MAX_RECORD_MS / 1000, DIALOG_SILENCE_MS);

    for (;;) {
        run_one_dialog();
    }
}

esp_err_t dialog_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(dialog_task, "dialog", 8192, NULL, 5, NULL, 1);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool dialog_is_speaking(void)
{
    return s_speaking;
}
