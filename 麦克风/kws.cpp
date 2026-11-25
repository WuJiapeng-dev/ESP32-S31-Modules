/*
 * kws.cpp - 端点检测 + 声学特征分类 + 可选 DTW 模板匹配
 *
 * 端点检测参数沿用 ESP-IDF 版实测值：
 *   预滚 300ms（30 帧）避免吃掉字头；连续 3 帧过阈才认为开口；
 *   连续 250ms 静音（25 帧）判定说完；最短 20 帧（200ms）否则丢弃。
 *   噪声底用 0.95/0.05 的一阶平滑跟踪，触发阈值 = max(3×噪声底, 180)。
 */
#include "kws.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <Preferences.h>

#include "dtw.h"
#include "mfcc.h"

#define KWS_MAX_FRAMES   250      /* 单句最长 2.5s */
#define KWS_PREROLL      30
#define KWS_SIL_HANG     25
#define KWS_TRIG_FRAMES   3
#define KWS_MIN_FRAMES   20
#define Q_SCALE          64.0f
#define DTW_ACCEPT       20.0f    /* 模板接受阈值，超过即认为不像 */

static const char *k_text[KWS_PHRASE_COUNT] = {
    "你好",
    "康神开播了，真的假的",
};

typedef struct {
    bool     used;
    uint16_t frames;
    int16_t  feat[KWS_MAX_FRAMES * MFCC_CEPS];
} tmpl_t;

static tmpl_t     s_tmpl[KWS_PHRASE_COUNT];
static Preferences s_prefs;

/* 采集与端点状态 */
static bool     s_rec;
static int      s_hit;
static int      s_sil;
static int16_t  s_pcm[MFCC_FRAME];
static float    s_rms[KWS_MAX_FRAMES];
static int      s_rms_n;
static int16_t  s_utt[KWS_MAX_FRAMES * MFCC_CEPS];
static int      s_utt_n;
static int16_t  s_pre[KWS_PREROLL * MFCC_CEPS];
static int      s_pre_head;
static int      s_pre_cnt;
static float    s_floor = 120.0f;

static int      s_enroll_id = -1;
static kws_cb_t s_cb;

/* ---------------- 模板持久化 ---------------- */
static void tmpl_load(void)
{
    if (!s_prefs.begin("kws", false)) {
        return;
    }
    for (int i = 0; i < KWS_PHRASE_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", i);
        size_t sz = s_prefs.getBytesLength(key);
        if (sz != sizeof(uint16_t) + sizeof(uint16_t) * KWS_MAX_FRAMES * MFCC_CEPS) {
            continue;
        }
        uint8_t *buf = (uint8_t *)malloc(sz);
        if (buf == NULL) {
            break;
        }
        if (s_prefs.getBytes(key, buf, sz) == sz) {
            memcpy(&s_tmpl[i].frames, buf, sizeof(uint16_t));
            if (s_tmpl[i].frames > 0 && s_tmpl[i].frames <= KWS_MAX_FRAMES) {
                memcpy(s_tmpl[i].feat, buf + sizeof(uint16_t),
                       (size_t)s_tmpl[i].frames * MFCC_CEPS * sizeof(int16_t));
                s_tmpl[i].used = true;
            }
        }
        free(buf);
    }
    s_prefs.end();
}

static bool tmpl_save(int id)
{
    if (!s_prefs.begin("kws", false)) {
        return false;
    }
    char key[8];
    snprintf(key, sizeof(key), "t%d", id);
    size_t sz = sizeof(uint16_t) + sizeof(uint16_t) * KWS_MAX_FRAMES * MFCC_CEPS;
    uint8_t *buf = (uint8_t *)calloc(1, sz);
    if (buf == NULL) {
        s_prefs.end();
        return false;
    }
    memcpy(buf, &s_tmpl[id].frames, sizeof(uint16_t));
    memcpy(buf + sizeof(uint16_t), s_tmpl[id].feat,
           (size_t)s_tmpl[id].frames * MFCC_CEPS * sizeof(int16_t));
    size_t w = s_prefs.putBytes(key, buf, sz);
    free(buf);
    s_prefs.end();
    return w == sz;
}

int kws_template_count(void)
{
    int n = 0;
    for (int i = 0; i < KWS_PHRASE_COUNT; i++) {
        if (s_tmpl[i].used) {
            n++;
        }
    }
    return n;
}

float kws_noise_floor(void)
{
    return s_floor;
}

void kws_clear_templates(void)
{
    for (int i = 0; i < KWS_PHRASE_COUNT; i++) {
        s_tmpl[i].used = false;
    }
    if (s_prefs.begin("kws", false)) {
        s_prefs.clear();
        s_prefs.end();
    }
    Serial.println("[识别] 模板已全部清除，回落到声学特征分类");
}

void kws_enroll(int id)
{
    if (id < 0 || id >= KWS_PHRASE_COUNT) {
        Serial.println("[识别] 无效短语编号");
        return;
    }
    s_enroll_id = id;
    Serial.printf("[识别] 请现在说：「%s」，等提示保存完成\r\n", k_text[id]);
}

/* ---------------- 特征工具 ---------------- */
static void pre_push(const int16_t *f)
{
    memcpy(s_pre + s_pre_head * MFCC_CEPS, f, MFCC_CEPS * sizeof(int16_t));
    s_pre_head = (s_pre_head + 1) % KWS_PREROLL;
    if (s_pre_cnt < KWS_PREROLL) {
        s_pre_cnt++;
    }
}

static void pre_flush(void)
{
    int start = (s_pre_head - s_pre_cnt + KWS_PREROLL) % KWS_PREROLL;
    for (int i = 0; i < s_pre_cnt && s_utt_n < KWS_MAX_FRAMES; i++) {
        int idx = (start + i) % KWS_PREROLL;
        memcpy(s_utt + s_utt_n * MFCC_CEPS, s_pre + idx * MFCC_CEPS,
               MFCC_CEPS * sizeof(int16_t));
        s_utt_n++;
    }
    s_pre_cnt  = 0;
    s_pre_head = 0;
}

/* 倒谱均值归一化：抹掉麦克风/房间带来的固定通道差异 */
static void cmn(int16_t *feat, int n)
{
    if (n <= 0) {
        return;
    }
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

/*
 * 数音节：对能量包络先做 3 点平滑，再找局部极大值。
 * 相邻峰至少间隔 6 帧（60ms）才算一个新音节，避免把一个字里的
 * 声母/韵母起伏数成两个字。
 */
static int count_syllables(const float *r, int n)
{
    if (n < 6) {
        return 0;
    }

    float mx = 0.0f;
    for (int i = 0; i < n; i++) {
        if (r[i] > mx) {
            mx = r[i];
        }
    }
    if (mx < 1.0f) {
        return 0;
    }

    const float lo = 0.18f * mx;
    int cnt  = 0;
    int last = -100;

    for (int i = 1; i < n - 1; i++) {
        float sm = (r[i - 1] + 2.0f * r[i] + r[i + 1]) * 0.25f;
        float l  = (i > 1) ? (r[i - 2] + 2.0f * r[i - 1] + r[i]) * 0.25f : r[i - 1];
        float rr = (i < n - 2) ? (r[i] + 2.0f * r[i + 1] + r[i + 2]) * 0.25f : r[i + 1];

        if (sm > lo && sm >= l && sm >= rr) {
            if (i - last >= 6) {
                cnt++;
            }
            last = i;
        }
    }
    return cnt;
}

/* 路径 A：声学特征分类（无需模板） */
static int classify_by_feature(int frames, int syl)
{
    if (syl >= 5 || frames >= 130) {
        return KWS_PHRASE_LONG;
    }
    if (syl <= 3 && frames <= 110) {
        return KWS_PHRASE_SHORT;
    }
    return -1;
}

static void on_utterance_done(void)
{
    int frames = s_utt_n;       /* 含 300ms 预滚，给 DTW 用 */
    int voiced = s_rms_n;       /* 不含预滚，衡量真实说话时长 */
    int syl    = count_syllables(s_rms, s_rms_n);

    if (voiced < KWS_MIN_FRAMES) {
        s_rms_n = 0;
        return;
    }

    cmn(s_utt, frames);

    /* 教学模式：直接存模板 */
    if (s_enroll_id >= 0) {
        tmpl_t *t = &s_tmpl[s_enroll_id];
        t->used   = true;
        t->frames = (uint16_t)frames;
        memcpy(t->feat, s_utt, (size_t)frames * MFCC_CEPS * sizeof(int16_t));

        if (tmpl_save(s_enroll_id)) {
            Serial.printf("[识别] 模板已保存：「%s」%d 帧\r\n",
                          k_text[s_enroll_id], frames);
        } else {
            Serial.println("[识别] 模板保存失败（NVS 写入错误）");
        }
        s_enroll_id = -1;
        s_rms_n = 0;
        return;
    }

    /* 路径 B 优先：有模板就用 DTW */
    int   best_id = -1;
    float best_d  = 1e9f;
    for (int i = 0; i < KWS_PHRASE_COUNT; i++) {
        if (!s_tmpl[i].used) {
            continue;
        }
        float d = dtw_distance(s_utt, frames, s_tmpl[i].feat, s_tmpl[i].frames, MFCC_CEPS);
        if (d < best_d) {
            best_d  = d;
            best_id = i;
        }
    }

    int hit = -1;
    if (best_id >= 0 && best_d <= DTW_ACCEPT) {
        hit = best_id;
    } else {
        hit = classify_by_feature(voiced, syl);
    }

    if (hit >= 0) {
        Serial.printf("[识别] 命中「%s」（语音 %d 帧 / %d 音节 / DTW=%.1f）\r\n",
                      k_text[hit], voiced, syl, (double)best_d);
        if (s_cb) {
            s_cb(hit, k_text[hit]);
        }
    } else {
        Serial.printf("[识别] 未匹配（语音 %d 帧 / %d 音节）\r\n", voiced, syl);
    }

    s_rms_n = 0;
}

void kws_begin(kws_cb_t cb)
{
    s_cb = cb;
    mfcc_init();
    tmpl_load();
    memset(s_pcm, 0, sizeof(s_pcm));
    Serial.printf("[识别] 就绪：%d 条模板，%s\r\n",
                  kws_template_count(),
                  kws_template_count() > 0 ? "优先走 DTW 模板匹配" : "走声学特征分类（无需训练）");
    Serial.printf("[识别] 待识别短语：1) %s   2) %s\r\n", k_text[0], k_text[1]);
}

void kws_feed(const int16_t *pcm, size_t n)
{
    if (pcm == NULL || n == 0) {
        return;
    }
    if (n > MFCC_HOP) {
        n = MFCC_HOP;
    }

    /* 400 点滑动窗：丢最老的一段，接上最新的 */
    memmove(s_pcm, s_pcm + MFCC_HOP, (MFCC_FRAME - MFCC_HOP) * sizeof(int16_t));
    memcpy(s_pcm + (MFCC_FRAME - MFCC_HOP), pcm, n * sizeof(int16_t));
    if (n < MFCC_HOP) {
        memset(s_pcm + (MFCC_FRAME - MFCC_HOP) + n, 0,
               (MFCC_HOP - n) * sizeof(int16_t));
    }

    float   rms = pcm_rms(pcm, (int)n);
    float   f[MFCC_CEPS];
    int16_t q[MFCC_CEPS];

    mfcc_compute(s_pcm, f);
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

    /* 噪声底自适应：只在没录音时更新 */
    if (!s_rec && rms < s_floor * 1.5f) {
        s_floor = 0.95f * s_floor + 0.05f * rms;
    }
    if (s_floor < 20.0f) {
        s_floor = 20.0f;
    }

    float thr = s_floor * 3.0f;
    if (thr < 180.0f) {
        thr = 180.0f;
    }

    if (!s_rec) {
        pre_push(q);
        if (rms > thr) {
            s_hit++;
        } else {
            s_hit = 0;
        }
        if (s_hit >= KWS_TRIG_FRAMES) {
            s_rec   = true;
            s_utt_n = 0;
            s_rms_n = 0;
            s_sil   = 0;
            pre_flush();
        }
        return;
    }

    /* 录音中 */
    if (s_utt_n < KWS_MAX_FRAMES) {
        memcpy(s_utt + s_utt_n * MFCC_CEPS, q, MFCC_CEPS * sizeof(int16_t));
        s_utt_n++;
    }
    if (s_rms_n < KWS_MAX_FRAMES) {
        s_rms[s_rms_n++] = rms;
    }

    if (rms < thr) {
        s_sil++;
    } else {
        s_sil = 0;
    }

    if (s_sil >= KWS_SIL_HANG || s_utt_n >= KWS_MAX_FRAMES) {
        s_rec = false;
        s_hit = 0;
        s_sil = 0;
        on_utterance_done();
    }
}
