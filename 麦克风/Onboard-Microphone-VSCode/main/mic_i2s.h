/*
 * mic_i2s.h - 麦克风采集（两种后端，编译期二选一）
 *
 * 默认后端 = 板载麦克风（ES8389 codec），对应当前这块板：
 *   慧勤智远 Waiken-Smart ESP32-S31 CB V1.1（原理图 Rev 2026-09-17）
 *   板载两颗模拟 MEMS 麦 LMA3729T381 → ES8389 的 ADC → I2S ASDOUT 进 ESP32。
 *   依据：厂商原理图第 1/3 页 + 用户手册 1.5 节引脚分配表。
 *
 * 另一路后端 = 外接 INMP441 数字麦，给没有 codec 的板子用，改一行宏即可。
 *
 * 为什么采样率都是 16kHz：
 *   语音识别用的 MFCC 在 8kHz 带宽内已经够用，16kHz 覆盖到 8kHz，
 *   同时把 FFT 点数、DMA 带宽、算力压到一半，适合 MCU 实时提特征。
 *   ES8389 的时钟系数表里有 {Ratio=32, MCLK=512000, LRCK=16000} 这一行，
 *   BCLK = 16kHz × 16bit × 2声道 = 512kHz，正好命中，不需要重采样。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MIC_BACKEND_ES8389  1
#define MIC_BACKEND_INMP441 2

/* ---------------- 选择采集后端（改这里） ---------------- */
#ifndef MIC_BACKEND
#define MIC_BACKEND MIC_BACKEND_ES8389
#endif

#define MIC_SAMPLE_RATE     16000

/* ---------------- ES8389（本板板载 codec） ----------------
 * 引脚全部取自慧勤智远 ESP32-S31 CB V1.1 原理图：
 *   I2C 与触摸、摄像头共用一条总线（板上有 4.7k 上拉 R1/R2 到 3.3V）
 *   ES8389 7bit 地址 0x10（8bit 写法 0x20），AD0 经 100k 下拉
 *   注意 SDA=0 / SCL=1，两个不能对调
 */
#define MIC_I2C_SDA_GPIO    0       /* ES8389 / 触摸 / 摄像头 共用 I2C SDA */
#define MIC_I2C_SCL_GPIO    1       /* 同上 SCL */
#define MIC_I2S_MCLK_GPIO   2       /* I2S_MCLK → codec（本方案不驱动，见下） */
#define MIC_I2S_BCLK_GPIO   3       /* I2S_SCLK / BCLK */
#define MIC_I2S_WS_GPIO     4       /* I2S_LRCK / WS */
#define MIC_I2S_SD_GPIO     6       /* I2S_ASDOUT：codec → ESP32（录音方向） */
#define MIC_PA_CTRL_GPIO    7       /* 两颗 NS4150B 的使能，高电平开；录音时拉低 */

/* 取哪个声道当单声道用：0=左(MIC1) 1=右(MIC2) 2=两路取平均
 * ES8389 是双 ADC，板上有两颗麦分别进 L/R；识别只用一路即可。 */
#ifndef MIC_CH_SEL
#define MIC_CH_SEL          0
#endif

/* 麦克风 PGA 增益（dB）。LMA3729 灵敏度约 -38dBV，30dB 起步，
 * 嫌小改 33.5 / 36，削波（电平顶到 ±32767）就往下调到 27.5 / 24.5 */
#ifndef MIC_PGA_DB
#define MIC_PGA_DB          30.0f
#endif

/* ---------------- INMP441（外接数字麦，备用后端） ---------------- */
#define MIC_I2S_SCK_GPIO    19      /* INMP441 SCK (BCLK)  */
#define MIC_INMP441_WS_GPIO 46      /* INMP441 WS  (LRCLK) */
#define MIC_INMP441_SD_GPIO 13      /* INMP441 SD  (DATA)  */

/* 现场调增益用：置 1 后识别任务每 500ms 打印一行电平（RMS/噪声底/阈值）。
 * 正常固件保持 0，避免刷屏。改完要重新编译烧录。 */
#ifndef MIC_LEVEL_DEBUG
#define MIC_LEVEL_DEBUG     0
#endif

esp_err_t mic_i2s_init(void);

/*
 * 读单声道 16bit 样本。
 * 返回实际读到的样本数（正常应等于 frames；无数据时返回 0）。
 * 注意：初始化失败时本函数恒返回 0，调用方必须能容忍。
 */
size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms);
