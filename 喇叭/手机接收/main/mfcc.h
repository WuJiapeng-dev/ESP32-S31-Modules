/*
 * mfcc.h - 13 维 MFCC 特征（16kHz / 25ms 帧长 / 10ms 帧移）
 *
 * 参数依据：
 *   - 25ms 帧长：语音短时平稳假设的经典取值（HTK/Kaldi 默认 25ms）
 *   - 10ms 帧移：与 100Hz 帧率对应，既能跟住音素变化，也不至于让模板序列过长
 *   - 26 个 Mel 三角滤波器、60–7600Hz：覆盖语音主要能量带，避开 INMP441 的直流与高频滚降
 *   - 13 维（丢掉 c0）：c0 是帧能量，受说话远近影响极大，丢掉后靠 CMN 做通道归一
 */
#pragma once

#include <stdint.h>

#define MFCC_CEPS       13
#define MFCC_FRAME      400     /* 25ms @ 16kHz */
#define MFCC_HOP        160     /* 10ms @ 16kHz */
#define MFCC_FILTERS    26

void mfcc_init(void);

/* pcm 长度必须是 MFCC_FRAME；out 输出 MFCC_CEPS 个系数 */
void mfcc_compute(const int16_t *pcm, float *out);

/* 短时 RMS，用于 VAD（端点检测） */
float pcm_rms(const int16_t *pcm, int n);
