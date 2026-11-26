/*
 * dtw.c - DTW 实现（带 Sakoe-Chiba 带宽约束）
 *
 * 为什么要带宽约束：不加约束时 DTW 允许任意"横着走"，两条不相关的语音
 * 也能对齐出很小的距离，识别会变得没有区分度；限制在 ±band 的对角带内，
 * 既保留正常语速差异，又挡住乱对齐。
 */
#include "dtw.h"

#include <stdlib.h>

#define DTW_MAX_LEN     256     /* 与 asr.h 的 ASR_MAX_FRAMES 对齐并留余量 */

static float s_row0[DTW_MAX_LEN + 1];
static float s_row1[DTW_MAX_LEN + 1];

#define DTW_INF 1e9f

static inline float frame_cost(const int16_t *x, const int16_t *y, int dim)
{
    int acc = 0;
    for (int i = 0; i < dim; i++) {
        acc += abs((int)x[i] - (int)y[i]);
    }
    return (float)acc / (float)dim;
}

static inline float min3(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

float dtw_distance(const int16_t *a, int na, const int16_t *b, int nb, int dim)
{
    if (a == NULL || b == NULL || na <= 0 || nb <= 0 || dim <= 0) {
        return DTW_INF;
    }
    if (na > DTW_MAX_LEN || nb > DTW_MAX_LEN) {
        return DTW_INF;
    }

    int band = abs(na - nb) + na / 8;
    if (band < 12) {
        band = 12;
    }

    float *prev = s_row0;
    float *cur  = s_row1;

    for (int j = 0; j <= nb; j++) {
        prev[j] = DTW_INF;
    }
    prev[0] = 0.0f;
    cur[0] = DTW_INF;

    for (int i = 1; i <= na; i++) {
        for (int j = 0; j <= nb; j++) {
            cur[j] = DTW_INF;
        }

        int center = (int)((long)i * (long)nb / (long)na);
        int jlo = center - band;
        int jhi = center + band;
        if (jlo < 1) {
            jlo = 1;
        }
        if (jhi > nb) {
            jhi = nb;
        }

        for (int j = jlo; j <= jhi; j++) {
            float c = frame_cost(a + (i - 1) * dim, b + (j - 1) * dim, dim);
            cur[j] = c + min3(prev[j], prev[j - 1], cur[j - 1]);
        }

        float *tmp = prev;
        prev = cur;
        cur = tmp;
    }

    float total = prev[nb];
    if (total >= DTW_INF) {
        return DTW_INF;
    }
    return total / (float)(na + nb);
}
