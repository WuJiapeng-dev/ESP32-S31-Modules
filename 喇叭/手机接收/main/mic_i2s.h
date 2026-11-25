/*
 * mic_i2s.h - 麦克风采集（两种后端，编译期二选一）
 *
 * 为什么留两套：
 *   按 xiaozhi 仓库的板级资料，ESP32-S31-Function-CoreBoard-1 是板载 ES8311 +
 *   板载麦克风。但本机实测（2026-11-25）在 I2C GPIO50/51 上扫描整条总线，
 *   应答设备数为 0 —— 手上这块板并没有 ES8311。所以默认走外接 INMP441，
 *   ES8311 那套代码完整保留，将来换到真正的 Function-CoreBoard 上改一行即可。
 *
 * 怎么切换：改下面 MIC_BACKEND 这一行，重新编译烧录。
 *
 * 为什么两者采样率都是 16kHz：
 *   语音识别用的 MFCC 在 8kHz 带宽内足够；16kHz 覆盖到 8kHz，
 *   同时把 FFT 点数、DMA 带宽、算力压到一半，适合 MCU 实时提特征。
 *   ES8311 的时钟分频表里有 {MCLK=4096000, rate=16000} 这一行，
 *   4.096MHz = 16kHz × 256 正好整除，所以不需要重采样。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MIC_BACKEND_ES8311  1
#define MIC_BACKEND_INMP441 2

/* ---------------- 选择采集后端（改这里） ---------------- */
#ifndef MIC_BACKEND
#define MIC_BACKEND MIC_BACKEND_INMP441
#endif

#define MIC_SAMPLE_RATE     16000

/* ---------------- INMP441（外接数字麦） ---------------- */
#define MIC_I2S_SCK_GPIO    19      /* INMP441 SCK (BCLK)  */
#define MIC_I2S_WS_GPIO     46      /* INMP441 WS  (LRCLK) */
#define MIC_I2S_SD_GPIO     13      /* INMP441 SD  (DATA)  */

/* ---------------- ES8311（板载 codec） ---------------- */
#define MIC_I2C_SCL_GPIO    50      /* ES8311 I2C SCL */
#define MIC_I2C_SDA_GPIO    51      /* ES8311 I2C SDA */
#define MIC_I2S_MCLK_GPIO   52      /* ES8311 MCLK  */
#define MIC_I2S_BCLK_GPIO   53      /* I2S BCLK / SCLK */
#define MIC_I2S_ES_WS_GPIO  55      /* I2S WS / LRCK */
#define MIC_I2S_ES_DIN_GPIO 54      /* ES8311 ASDOUT → ESP32（录音） */

esp_err_t mic_i2s_init(void);

/*
 * 读单声道 16bit 样本。
 * 返回实际读到的样本数（正常应等于 frames；无数据时返回 0）。
 * 注意：初始化失败时本函数恒返回 0，调用方必须能容忍。
 */
size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms);
