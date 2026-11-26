/*
 * mfcc.c - MFCC 特征提取
 *
 * 流程：预加重(0.97) → 汉明窗 → 512 点 FFT → 功率谱 → 26 带 Mel 三角滤波
 *      → 取对数 → DCT-II 取前 13 个（跳过 c0）
 *
 * 缓冲区全部用 static：调用方是单个 asr 任务，不需要可重入；
 * 这样每帧省下 5KB 栈空间，任务栈可以保持 4KB。
 */
#include "mfcc.h"

#include <math.h>
#include <stdbool.h>

#include "fft.h"

#define F_LOW_HZ        60.0f
#define F_HIGH_HZ       7600.0f
#define MIC_SR_F        16000.0f    /* 与 mic_i2s.h 的 MIC_SAMPLE_RATE 保持一致 */

static float s_win[MFCC_FRAME];
static int   s_fb_lo[MFCC_FILTERS];
static int   s_fb_ctr[MFCC_FILTERS];
static int   s_fb_hi[MFCC_FILTERS];
static float s_dct[MFCC_CEPS][MFCC_FILTERS];
static bool  s_ready;

static float s_re[FFT_N];
static float s_im[FFT_N];
static float s_pw[FFT_N / 2 + 1];
static float s_lm[MFCC_FILTERS];


static float hz2mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel2hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

void mfcc_init(void)
{
    /* 汉明窗 */
    for (int i = 0; i < MFCC_FRAME; i++) {
        s_win[i] = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(MFCC_FRAME - 1));
    }

    /* Mel 滤波器组边界（26 个三角，共 28 条边界） */
    float mel_lo = hz2mel(F_LOW_HZ);
    float mel_hi = hz2mel(F_HIGH_HZ);
    int   edge[MFCC_FILTERS + 2];

    for (int i = 0; i < MFCC_FILTERS + 2; i++) {
        float mel = mel_lo + (mel_hi - mel_lo) * (float)i / (float)(MFCC_FILTERS + 1);
        float hz  = mel2hz(mel);
        int   bin = (int)(hz / (float)MIC_SR_F * (float)FFT_N + 0.5f);
        if (bin < 0) {
            bin = 0;
        }
        if (bin > FFT_N / 2) {
            bin = FFT_N / 2;
        }
        edge[i] = bin;
    }

    for (int j = 0; j < MFCC_FILTERS; j++) {
        s_fb_lo[j]  = edge[j];
        s_fb_ctr[j] = edge[j + 1];
        s_fb_hi[j]  = edge[j + 2];
        /* 防止相邻边界重合导致除零 */
        if (s_fb_ctr[j] <= s_fb_lo[j]) {
            s_fb_ctr[j] = s_fb_lo[j] + 1;
        }
        if (s_fb_hi[j] <= s_fb_ctr[j]) {
            s_fb_hi[j] = s_fb_ctr[j] + 1;
        }
        if (s_fb_hi[j] > FFT_N / 2) {
            s_fb_hi[j] = FFT_N / 2;
        }
    }

    /* DCT-II 表：c_i = sum_j lm[j] * cos(pi*i*(j+0.5)/N)，i 从 1 开始（跳过 c0） */
    for (int i = 0; i < MFCC_CEPS; i++) {
        for (int j = 0; j < MFCC_FILTERS; j++) {
            s_dct[i][j] = cosf((float)M_PI * (float)(i + 1) *
                               ((float)j + 0.5f) / (float)MFCC_FILTERS);
        }
    }

    fft_init();
    s_ready = true;
}

void mfcc_compute(const int16_t *pcm, float *out)
{
    if (!s_ready) {
        mfcc_init();
    }

    /* 预加重 + 加窗 + 补零到 512 */
    float prev = (float)pcm[0];
    for (int i = 0; i < MFCC_FRAME; i++) {
        float x = (float)pcm[i];
        s_re[i] = (x - 0.97f * prev) * s_win[i];
        s_im[i] = 0.0f;
        prev = x;
    }
    for (int i = MFCC_FRAME; i < FFT_N; i++) {
        s_re[i] = 0.0f;
        s_im[i] = 0.0f;
    }

    fft_run(s_re, s_im);

    for (int k = 0; k <= FFT_N / 2; k++) {
        s_pw[k] = s_re[k] * s_re[k] + s_im[k] * s_im[k];
    }

    /* Mel 三角滤波 + 取对数 */
    for (int j = 0; j < MFCC_FILTERS; j++) {
        float sum = 0.0f;
        int   lo  = s_fb_lo[j];
        int   ct  = s_fb_ctr[j];
        int   hi  = s_fb_hi[j];

        for (int k = lo; k < ct; k++) {
            sum += s_pw[k] * ((float)(k - lo) / (float)(ct - lo));
        }
        for (int k = ct; k < hi; k++) {
            sum += s_pw[k] * ((float)(hi - k) / (float)(hi - ct));
        }
        s_lm[j] = logf(sum + 1e-6f);
    }

    /* DCT 取 13 维 */
    for (int i = 0; i < MFCC_CEPS; i++) {
        float acc = 0.0f;
        for (int j = 0; j < MFCC_FILTERS; j++) {
            acc += s_lm[j] * s_dct[i][j];
        }
        out[i] = acc;
    }
}

float pcm_rms(const int16_t *pcm, int n)
{
    if (n <= 0) {
        return 0.0f;
    }
    float acc = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = (float)pcm[i];
        acc += v * v;
    }
    return sqrtf(acc / (float)n);
}
