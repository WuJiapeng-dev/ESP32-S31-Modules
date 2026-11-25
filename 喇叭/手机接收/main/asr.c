/*
 * asr.c - 流式端点检测 + MFCC + DTW 模板匹配 + 模板持久化（不接喇叭）
 *
 * 与 ../复述/main/asr.c 的差异：
 *   - 没有 tts_say()/tts_busy()：所有反馈只走 BLE Notify
 *   - 没有"播报期间暂停收音"的逻辑（无声学回授，不需要）
 *   - 命令集去掉 SAY（没喇叭），加 STATE/VER
 *
 * 数据流（全在 asr 任务里串起来）：
 *   I2S 每 10ms 交 160 样本 → 拼 400 点窗口 → 算 MFCC（int16 量化）
 *     → 预滚环 300ms → 触发后进语句缓冲（最多 2s）
 *     → 静音 250ms 认为说完 → CMN → DTW 比对 → Notify
 */
#include "asr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ble_bridge.h"
#include "dtw.h"
#include "mfcc.h"
#include "mic_i2s.h"

static const char *TAG = "asr";

#define ASR_VER             "phone_recv/1.0"

#define ASR_MAX_TMPL        8       /* 8×200×13×2B ≈ 42KB */
#define ASR_MAX_FRAMES      200     /* 单句最长 2.0s（10ms/帧） */
#define ASR_PREROLL         30      /* 300ms 预滚，避免吃掉字头 */
#define ASR_SIL_HANG        25      /* 连续 250ms 静音判定说完 */
#define ASR_TRIG_FRAMES      3      /* 连续 30ms 过阈值才开始 */
#define ASR_MIN_FRAMES      25      /* 少于 250ms 丢弃 */
#define Q_SCALE             64.0f
#define NVS_NS              "asr"

typedef struct {
    char     label[40];
    uint16_t frames;
    uint16_t dim;
} tmpl_hdr_t;

typedef struct {
    bool     used;
    char     label[40];
    uint16_t frames;
    int16_t  feat[ASR_MAX_FRAMES * MFCC_CEPS];
} tmpl_t;

typedef enum {
    MODE_OFF = 0,
    MODE_LISTEN,
    MODE_ENROLL,
} asr_mode_t;

static tmpl_t   s_tmpl[ASR_MAX_TMPL];
static int16_t  s_utt[ASR_MAX_FRAMES * MFCC_CEPS];
static int      s_utt_n;
static int16_t  s_pre[ASR_PREROLL * MFCC_CEPS];
static int      s_pre_head;
static int      s_pre_cnt;

static volatile asr_mode_t s_mode = MODE_OFF;
static char     s_enroll_label[40];
static int      s_enroll_slot = -1;
static float    s_threshold = 18.0f;
static float    s_floor = 80.0f;

#define CMD_MAX      200
#define CMD_IDLE_US  (250 * 1000)

static char             s_cmd[CMD_MAX];
static volatile size_t  s_cmd_len;
static volatile int64_t s_cmd_ts;
static portMUX_TYPE     s_cmd_lock = portMUX_INITIALIZER_UNLOCKED;

void app_cmd_feed(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_cmd_lock);
    size_t space = CMD_MAX - 1 - s_cmd_len;
    if (len > space) {
        len = space;
    }
    if (len > 0) {
        memcpy(s_cmd + s_cmd_len, data, len);
        s_cmd_len += len;
        s_cmd[s_cmd_len] = '\0';
        s_cmd_ts = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_cmd_lock);
}

static bool cmd_take(char *out, size_t out_size)
{
    bool got = false;

    portENTER_CRITICAL(&s_cmd_lock);
    if (s_cmd_len > 0 && (esp_timer_get_time() - s_cmd_ts) >= CMD_IDLE_US) {
        size_t n = s_cmd_len;
        if (n > out_size - 1) {
            n = out_size - 1;
        }
        memcpy(out, s_cmd, n);
        out[n] = '\0';
        s_cmd_len = 0;
        got = true;
    }
    portEXIT_CRITICAL(&s_cmd_lock);

    return got;
}

/* ---------------- 模板持久化 ---------------- */
static int tmpl_count(void)
{
    int n = 0;
    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        if (s_tmpl[i].used) {
            n++;
        }
    }
    return n;
}

static int tmpl_find_label(const char *label)
{
    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        if (s_tmpl[i].used && strcmp(s_tmpl[i].label, label) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t tmpl_save(int idx)
{
    tmpl_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strlcpy(hdr.label, s_tmpl[idx].label, sizeof(hdr.label));
    hdr.frames = s_tmpl[idx].frames;
    hdr.dim    = MFCC_CEPS;

    size_t payload = (size_t)hdr.frames * MFCC_CEPS * sizeof(int16_t);
    size_t total   = sizeof(hdr) + payload;

    uint8_t *buf = malloc(total);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), s_tmpl[idx].feat, payload);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", idx);
        err = nvs_set_blob(h, key, buf, total);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }

    free(buf);
    return err;
}

static esp_err_t tmpl_erase(int idx)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    snprintf(key, sizeof(key), "t%d", idx);
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void tmpl_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS 无 %s 命名空间，模板为空", NVS_NS);
        return;
    }

    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", i);

        size_t sz = 0;
        if (nvs_get_blob(h, key, NULL, &sz) != ESP_OK || sz < sizeof(tmpl_hdr_t)) {
            continue;
        }

        uint8_t *buf = malloc(sz);
        if (buf == NULL) {
            break;
        }

        if (nvs_get_blob(h, key, buf, &sz) == ESP_OK) {
            tmpl_hdr_t hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            size_t need = (size_t)hdr.frames * MFCC_CEPS * sizeof(int16_t);

            if (hdr.dim == MFCC_CEPS && hdr.frames > 0 &&
                hdr.frames <= ASR_MAX_FRAMES && sz >= sizeof(hdr) + need) {
                s_tmpl[i].used   = true;
                s_tmpl[i].frames = hdr.frames;
                memcpy(s_tmpl[i].label, hdr.label, sizeof(s_tmpl[i].label));
                s_tmpl[i].label[sizeof(s_tmpl[i].label) - 1] = '\0';
                memcpy(s_tmpl[i].feat, buf + sizeof(hdr), need);
            }
        }
        free(buf);
    }

    size_t tsz = sizeof(float);
    float  thr = 0.0f;
    if (nvs_get_blob(h, "thr", &thr, &tsz) == ESP_OK && thr > 0.0f) {
        s_threshold = thr;
    }

    nvs_close(h);
}

static void threshold_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, "thr", &s_threshold, sizeof(s_threshold)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---------------- 特征工具 ---------------- */
static void feat_push(int16_t *dst, int *n, const int16_t *f)
{
    if (*n >= ASR_MAX_FRAMES) {
        return;
    }
    memcpy(dst + (*n) * MFCC_CEPS, f, MFCC_CEPS * sizeof(int16_t));
    (*n)++;
}

static void pre_push(const int16_t *f)
{
    memcpy(s_pre + s_pre_head * MFCC_CEPS, f, MFCC_CEPS * sizeof(int16_t));
    s_pre_head = (s_pre_head + 1) % ASR_PREROLL;
    if (s_pre_cnt < ASR_PREROLL) {
        s_pre_cnt++;
    }
}

static void pre_flush_to_utt(void)
{
    int start = (s_pre_head - s_pre_cnt + ASR_PREROLL) % ASR_PREROLL;
    for (int i = 0; i < s_pre_cnt; i++) {
        int idx = (start + i) % ASR_PREROLL;
        feat_push(s_utt, &s_utt_n, s_pre + idx * MFCC_CEPS);
    }
    s_pre_cnt  = 0;
    s_pre_head = 0;
}

static void cmn(int16_t *feat, int n)
{
    for (int d = 0; d < MFCC_CEPS; d++) {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            acc += feat[i * MFCC_CEPS + d];
        }
        int mean = acc / n;
        for (int i = 0; i < n; i++) {
            feat[i * MFCC_CEPS + d] = (int16_t)(feat[i * MFCC_CEPS + d] - mean);
        }
    }
}

/* ---------------- 一句说完 ---------------- */
static void on_utterance_done(void)
{
    if (s_utt_n < ASR_MIN_FRAMES) {
        ESP_LOGI(TAG, "语句过短(%d 帧)，忽略", s_utt_n);
        return;
    }

    ESP_LOGI(TAG, "语句结束: %d 帧 (%dms)", s_utt_n, s_utt_n * 10);
    cmn(s_utt, s_utt_n);

    if (s_mode == MODE_ENROLL && s_enroll_slot >= 0) {
        tmpl_t *t = &s_tmpl[s_enroll_slot];
        t->used   = true;
        t->frames = (uint16_t)s_utt_n;
        strlcpy(t->label, s_enroll_label, sizeof(t->label));
        memcpy(t->feat, s_utt, (size_t)s_utt_n * MFCC_CEPS * sizeof(int16_t));

        char msg[64];
        if (tmpl_save(s_enroll_slot) == ESP_OK) {
            snprintf(msg, sizeof(msg), "OK:ENROLLED %s", t->label);
            ESP_LOGI(TAG, "模板已保存: [%d] %s, %d 帧", s_enroll_slot, t->label, t->frames);
        } else {
            snprintf(msg, sizeof(msg), "ERR:SAVE");
            ESP_LOGE(TAG, "模板写入 NVS 失败");
        }
        ble_bridge_notify(msg);

        s_enroll_slot = -1;
        s_mode = MODE_LISTEN;
        return;
    }

    float best = 1e9f;
    int   best_i = -1;
    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        if (!s_tmpl[i].used) {
            continue;
        }
        float d = dtw_distance(s_utt, s_utt_n, s_tmpl[i].feat, s_tmpl[i].frames, MFCC_CEPS);
        if (d < best) {
            best   = d;
            best_i = i;
        }
    }

    char msg[80];
    if (best_i >= 0 && best <= s_threshold) {
        /* 先把文字推给手机——这是本工程唯一的产出 */
        snprintf(msg, sizeof(msg), "ASR:%s", s_tmpl[best_i].label);
        ble_bridge_notify(msg);

        snprintf(msg, sizeof(msg), "SCORE:%.1f", best);
        ble_bridge_notify(msg);

        ESP_LOGI(TAG, "命中: %s (d=%.1f <= %.1f)", s_tmpl[best_i].label, best, s_threshold);
    } else if (best_i >= 0) {
        snprintf(msg, sizeof(msg), "ASR:? d=%.1f th=%.1f", best, s_threshold);
        ble_bridge_notify(msg);
        ESP_LOGI(TAG, "未命中: 最近的是 %s, d=%.1f > %.1f",
                 s_tmpl[best_i].label, best, s_threshold);
    } else {
        ble_bridge_notify("ASR:? NOTMPL");
        ESP_LOGW(TAG, "没有模板，先用 EN:<标签> 教一句");
    }
}

/* ---------------- 命令 ---------------- */
static void cmd_list(void)
{
    char msg[64];
    int  n = tmpl_count();
    snprintf(msg, sizeof(msg), "LIST:%d", n);
    ble_bridge_notify(msg);

    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        if (!s_tmpl[i].used) {
            continue;
        }
        snprintf(msg, sizeof(msg), "T%d:%s", i + 1, s_tmpl[i].label);
        ble_bridge_notify(msg);
    }
}

static void cmd_del(const char *arg)
{
    int idx = -1;

    if (arg[0] >= '0' && arg[0] <= '9') {
        int n = atoi(arg);
        if (n >= 1 && n <= ASR_MAX_TMPL) {
            idx = n - 1;
        }
    } else {
        idx = tmpl_find_label(arg);
    }

    if (idx < 0 || idx >= ASR_MAX_TMPL || !s_tmpl[idx].used) {
        ble_bridge_notify("ERR:NOSUCH");
        return;
    }

    char label[40];
    strlcpy(label, s_tmpl[idx].label, sizeof(label));
    tmpl_erase(idx);
    memset(&s_tmpl[idx], 0, sizeof(s_tmpl[idx]));

    char msg[64];
    snprintf(msg, sizeof(msg), "OK:DEL %s", label);
    ble_bridge_notify(msg);
    ESP_LOGI(TAG, "已删除模板 %d (%s)", idx, label);
}

static void cmd_clear(void)
{
    for (int i = 0; i < ASR_MAX_TMPL; i++) {
        if (s_tmpl[i].used) {
            tmpl_erase(i);
        }
    }
    memset(s_tmpl, 0, sizeof(s_tmpl));
    ble_bridge_notify("OK:CLEAR");
    ESP_LOGI(TAG, "全部模板已清空");
}

static void cmd_enroll(const char *arg)
{
    int slot = -1;

    if (arg != NULL && arg[0] != '\0') {
        strlcpy(s_enroll_label, arg, sizeof(s_enroll_label));
        slot = tmpl_find_label(s_enroll_label);
    } else {
        snprintf(s_enroll_label, sizeof(s_enroll_label), "语句%d", tmpl_count() + 1);
        slot = tmpl_find_label(s_enroll_label);
    }

    if (slot < 0) {
        for (int i = 0; i < ASR_MAX_TMPL; i++) {
            if (!s_tmpl[i].used) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        ble_bridge_notify("ERR:FULL");
        ESP_LOGW(TAG, "模板已满（%d 条），先 DEL 再教", ASR_MAX_TMPL);
        return;
    }

    s_enroll_slot = slot;
    s_mode        = MODE_ENROLL;

    char msg[64];
    snprintf(msg, sizeof(msg), "OK:ENROLLING %s", s_enroll_label);
    ble_bridge_notify(msg);
    ESP_LOGI(TAG, "准备录入模板 [%d] %s，请立即说这句话", slot, s_enroll_label);
}

static void cmd_state(void)
{
    char msg[80];
    const char *m = (s_mode == MODE_LISTEN) ? "LISTEN" :
                    (s_mode == MODE_ENROLL) ? "ENROLL" : "OFF";
    snprintf(msg, sizeof(msg), "STATE:%s tmpl=%d th=%.1f",
             m, tmpl_count(), s_threshold);
    ble_bridge_notify(msg);
}

static void cmd_handle(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    ESP_LOGI(TAG, "收到命令: %s", s);

    if (strcasecmp(s, "STOP") == 0 || strncasecmp(s, "ASR:OFF", 7) == 0) {
        s_mode = MODE_OFF;
        ble_bridge_notify("OK:ASROFF");
        return;
    }
    if (strcasecmp(s, "ASR") == 0 || strncasecmp(s, "ASR:ON", 6) == 0) {
        s_mode = MODE_LISTEN;
        char msg[48];
        snprintf(msg, sizeof(msg), "OK:ASRON %d", tmpl_count());
        ble_bridge_notify(msg);
        return;
    }
    if (strcasecmp(s, "PING") == 0) {
        ble_bridge_notify("PONG");
        return;
    }
    if (strcasecmp(s, "VER") == 0) {
        ble_bridge_notify("VER:" ASR_VER);
        return;
    }
    if (strcasecmp(s, "STATE") == 0) {
        cmd_state();
        return;
    }
    if (strcasecmp(s, "LIST") == 0) {
        cmd_list();
        return;
    }
    if (strncasecmp(s, "DEL:", 4) == 0) {
        cmd_del(s + 4);
        return;
    }
    if (strcasecmp(s, "CLEAR") == 0 || strcasecmp(s, "CLEAR:ALL") == 0) {
        cmd_clear();
        return;
    }
    if (strncasecmp(s, "TH:", 3) == 0) {
        float v = strtof(s + 3, NULL);
        if (v > 0.5f && v < 200.0f) {
            s_threshold = v;
            threshold_save();
            ble_bridge_notify("OK:TH");
        } else {
            ble_bridge_notify("ERR:RANGE");
        }
        return;
    }
    if (strncasecmp(s, "EN:", 3) == 0) {
        cmd_enroll(s + 3);
        return;
    }

    /* 没有喇叭，非命令文本无法"播报"，明确拒绝而不是静默吞掉 */
    ble_bridge_notify("ERR:UNKNOWN");
}

/* ---------------- 主任务 ---------------- */
static void asr_task(void *arg)
{
    (void)arg;

    static int16_t pcm[MFCC_FRAME];
    static int16_t blk[MFCC_HOP];
    float     f[MFCC_CEPS];
    int16_t   q[MFCC_CEPS];

    bool rec = false;
    int  hit = 0;
    int  sil = 0;

    memset(pcm, 0, sizeof(pcm));

    ESP_LOGI(TAG, "识别任务启动（默认关闭，BLE 发 ASR 打开）");

    for (;;) {
        char line[CMD_MAX];
        while (cmd_take(line, sizeof(line))) {
            cmd_handle(line);
        }

        size_t got = mic_i2s_read(blk, MFCC_HOP, 100);
        if (got == 0) {
            /* 必须传 tick 数而不是 ms：CONFIG_FREERTOS_HZ=100 时
               pdMS_TO_TICKS(5) == 0，vTaskDelay(0) 只让出 CPU 立刻重跑，
               会把 CPU0 的 IDLE 饿死并触发 task_wdt。1 tick = 10ms，
               正好等于一帧 MFCC 的周期。 */
            vTaskDelay(1);
            continue;
        }
        if (got < MFCC_HOP) {
            memset(blk + got, 0, (MFCC_HOP - got) * sizeof(int16_t));
        }

        if (s_mode == MODE_OFF) {
            rec = false;
            hit = 0;
            sil = 0;
            s_pre_cnt  = 0;
            s_pre_head = 0;
            s_utt_n    = 0;
            continue;
        }

        memmove(pcm, pcm + MFCC_HOP, (MFCC_FRAME - MFCC_HOP) * sizeof(int16_t));
        memcpy(pcm + (MFCC_FRAME - MFCC_HOP), blk, MFCC_HOP * sizeof(int16_t));

        float rms = pcm_rms(blk, MFCC_HOP);
        mfcc_compute(pcm, f);

        for (int i = 0; i < MFCC_CEPS; i++) {
            float v = f[i] * Q_SCALE;
            if (v > 30000.0f) {
                v = 30000.0f;
            }
            if (v < -30000.0f) {
                v = -30000.0f;
            }
            q[i] = (int16_t)v;
        }

        if (!rec && rms < s_floor * 1.5f) {
            s_floor = 0.95f * s_floor + 0.05f * rms;
        }
        if (s_floor < 20.0f) {
            s_floor = 20.0f;
        }

        float thr = s_floor * 3.0f;
        if (thr < 180.0f) {
            thr = 180.0f;
        }

        if (!rec) {
            pre_push(q);
            if (rms > thr) {
                hit++;
            } else {
                hit = 0;
            }
            if (hit >= ASR_TRIG_FRAMES) {
                rec = true;
                s_utt_n = 0;
                sil = 0;
                pre_flush_to_utt();
                ESP_LOGI(TAG, "检测到说话，开始记录（噪声底 %.0f，阈值 %.0f）", s_floor, thr);
            }
        } else {
            feat_push(s_utt, &s_utt_n, q);
            if (rms < thr) {
                sil++;
            } else {
                sil = 0;
            }
            if (sil >= ASR_SIL_HANG || s_utt_n >= ASR_MAX_FRAMES) {
                rec = false;
                on_utterance_done();
            }
        }
    }
}

esp_err_t asr_init(void)
{
    s_cmd_len = 0;
    s_cmd[0]  = '\0';
    s_cmd_ts  = esp_timer_get_time();

    mfcc_init();
    tmpl_load_all();

    if (xTaskCreate(asr_task, "asr", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建识别任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "离线识别就绪：模板 %d/%d，阈值 %.1f",
             tmpl_count(), ASR_MAX_TMPL, s_threshold);
    return ESP_OK;
}
