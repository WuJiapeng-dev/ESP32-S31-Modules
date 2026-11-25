/*
 * fft.c - 512 点基 2 FFT 实现
 *
 * 旋转因子表 + 位反转表在 fft_init() 里算一次，避免每帧调用 sinf/cosf：
 * 100 帧/秒 × 9 级 × 256 个因子，如果实时算就是每秒 23 万次三角函数，
 * 在 240MHz 的核上会白白吃掉 10% 以上的 CPU。
 */
#include "fft.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_HALF (FFT_N / 2)

static float    s_cos[FFT_HALF];
static float    s_sin[FFT_HALF];
static uint16_t s_rev[FFT_N];
static bool     s_ready;

void fft_init(void)
{
    for (int i = 0; i < FFT_HALF; i++) {
        double a = 2.0 * M_PI * (double)i / (double)FFT_N;
        s_cos[i] = (float)cos(a);
        s_sin[i] = (float)sin(a);
    }

    int bits = 0;
    while ((1 << bits) < FFT_N) {
        bits++;
    }
    for (int i = 0; i < FFT_N; i++) {
        uint16_t r = 0;
        for (int b = 0; b < bits; b++) {
            if (i & (1 << b)) {
                r |= (uint16_t)(1 << (bits - 1 - b));
            }
        }
        s_rev[i] = r;
    }

    s_ready = true;
}

void fft_run(float *re, float *im)
{
    if (!s_ready) {
        fft_init();
    }

    /* 位反转重排 */
    for (int i = 0; i < FFT_N; i++) {
        int j = s_rev[i];
        if (j > i) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    /* 蝶形运算 */
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1;
        int step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int j = 0; j < half; j++) {
                int   k  = j * step;
                float wr = s_cos[k];
                float wi = -s_sin[k];       /* e^{-j2πkn/N} */

                float xr = re[i + j + half] * wr - im[i + j + half] * wi;
                float xi = re[i + j + half] * wi + im[i + j + half] * wr;

                re[i + j + half] = re[i + j] - xr;
                im[i + j + half] = im[i + j] - xi;
                re[i + j] += xr;
                im[i + j] += xi;
            }
        }
    }
}
