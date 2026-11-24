/*
 * mic_i2s.h - INMP441 麦克风采集（I2S 标准模式 / Philips / 16kHz）
 *
 * 为什么用 I2S 标准模式而不是 PDM：
 *   INMP441 是一颗数字 MEMS 麦克风，输出的是 I2S（不是 PDM）格式的 24bit 数据，
 *   所以 ESP32-S31 只需要做 I2S 主机发时钟，不需要 PDM 解码。
 *
 * 为什么 16kHz：
 *   语音识别用的 MFCC 特征在 8kHz 带宽内已足够（电话语音就是 8kHz 采样），
 *   16kHz 能覆盖到 8kHz，同时把 FFT 点数、DMA 带宽、算力都压到一半，
 *   相比 48kHz 更适合在 MCU 上做实时特征提取。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 用户填写的接线，逐字采用 */
#define MIC_I2S_SCK_GPIO    19      /* INMP441 SCK (BCLK)  */
#define MIC_I2S_WS_GPIO     46      /* INMP441 WS  (LRCLK) */
#define MIC_I2S_SD_GPIO     13      /* INMP441 SD  (DATA)  */

#define MIC_SAMPLE_RATE     16000

esp_err_t mic_i2s_init(void);

/*
 * 读单声道 16bit 样本。
 * 返回实际读到的样本数（正常应等于 frames；I2S 无数据时会提前返回）。
 */
size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms);
