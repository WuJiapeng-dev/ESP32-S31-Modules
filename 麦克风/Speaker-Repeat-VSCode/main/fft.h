/*
 * fft.h - 定长 512 点基 2 复数 FFT（自己实现，不引第三方库）
 *
 * 为什么自己写：本工程只需要一个 512 点、float、定长的正变换，
 * 引 esp-dsp 会多一个组件依赖（而且 esp-dsp 对 esp32s31 这种 preview 目标
 * 未必有优化实现），自己写 60 行就够，且能确定它用的是纯 C 浮点。
 */
#pragma once

#define FFT_N 512

/* 预计算旋转因子与位反转表，只需调用一次 */
void fft_init(void);

/* 原地变换：re/im 长度均为 FFT_N。输入时域，输出频域（未归一化） */
void fft_run(float *re, float *im);
