/*
 * audio_out.h - 音频输出，两种后端二选一
 *
 * ============================ 后端 A：PDM + 模拟功放（默认）============================
 * 适用：PAM8403 / PAM8302 / LM386 / TDA2822 这类「模拟输入」功放板。
 *
 *    GPIO17 --[R1 1k]--+--[C2 1uF]--> 功放 L IN
 *                      |
 *                    [C1 10nF]
 *                      |
 *                     GND
 *
 *    功放 5V 供电，GND 必须与 ESP32 共地；OUT+ / OUT- 直接接无源喇叭。
 *
 * 为什么必须加 RC：
 *   ESP32-S31 没有内置 DAC（soc_caps.h 里没有 SOC_DAC_SUPPORTED），
 *   只能走 I2S 的 PDM DAC 模式：软件写 16bit PCM，硬件 PCM2PDM 滤波器
 *   把它调制成 1bit 脉冲流从引脚输出，占空比正比于样本值。
 *   引脚上是 3.07MHz 的脉冲，RC 低通（截止 ≈15.9kHz）积分后才变回模拟波形。
 *   不接 RC 直接进功放 = 满耳朵高频啸叫 + 可能干扰功放工作点。
 *
 * 为什么软件要衰减：
 *   PAM8403 增益固定 24dB（无外部可调），满功率只需要约 0.28Vpp 输入；
 *   ESP32 摆幅 3.3Vpp，直连必然削波。AUDIO_PDM_ATTEN_SHIFT 默认 3（即 -18dB）。
 *   觉得声音小就把它改小（2 = -12dB），觉得破音就改大（4 = -24dB）。
 *
 * ============================ 后端 B：I2S + 数字功放 ============================
 * 适用：MAX98357A / PCM5102 这类 I2S 数字功放模块。
 *
 *    GPIO15 -> BCLK    GPIO16 -> LRC(WS)    GPIO17 -> DIN
 *    3V3    -> VIN     GND    -> GND
 *
 * 优点：不需要任何外围元件，没有载波残留，音质明显更好。
 * 缺点：要单独买模块。
 *
 * 切换方式：只改下面 AUDIO_OUT_BACKEND 一个数字，业务代码（speak.c 等）不用动。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_BACKEND_PDM   1   /* PDM -> RC 低通 -> 模拟功放 */
#define AUDIO_BACKEND_I2S   2   /* I2S 标准模式 -> 数字功放 */

#ifndef AUDIO_OUT_BACKEND
#define AUDIO_OUT_BACKEND   AUDIO_BACKEND_PDM
#endif

/* DeepSeek format=pcm 固定 24kHz、16bit、单声道，不可调 */
#define AUDIO_SAMPLE_RATE       24000

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM

/* PDM 数据引脚：一根线就够，接 RC 滤波后进功放 L IN */
#define AUDIO_PDM_DATA_GPIO     17
/* PDM 时钟引脚：PDM DAC 模式下也会输出时钟，但不需要接，随便给一个空闲脚占位 */
#define AUDIO_PDM_CLK_GPIO      16
/* 数字衰减：样本右移位数，3 => 1/8 => -18dB */
#define AUDIO_PDM_ATTEN_SHIFT   3

#else   /* AUDIO_BACKEND_I2S */

#define AUDIO_I2S_BCLK_GPIO     15
#define AUDIO_I2S_LRC_GPIO      16
#define AUDIO_I2S_DIN_GPIO      17

#endif

/* 启动日志用的一行描述，避免 main.c 里写条件编译 */
#define _AUDIO_STR_(x)          #x
#define _AUDIO_STR(x)           _AUDIO_STR_(x)

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM
#define AUDIO_OUT_DESC  "PDM DOUT=GPIO" _AUDIO_STR(AUDIO_PDM_DATA_GPIO) \
                        " -> RC 低通 -> 模拟功放(PAM8403), 数字衰减 >>" \
                        _AUDIO_STR(AUDIO_PDM_ATTEN_SHIFT)
#else
#define AUDIO_OUT_DESC  "I2S BCLK=GPIO" _AUDIO_STR(AUDIO_I2S_BCLK_GPIO) \
                        " LRC=GPIO" _AUDIO_STR(AUDIO_I2S_LRC_GPIO) \
                        " DIN=GPIO" _AUDIO_STR(AUDIO_I2S_DIN_GPIO) \
                        " -> 数字功放(MAX98357A)"
#endif

esp_err_t audio_out_init(void);

/* 写入单声道 16bit PCM（samples 为样本个数，不是字节数）。阻塞直到 DMA 吞吐完 */
esp_err_t audio_out_write_mono16(const int16_t *pcm, size_t samples);

/* 等 DMA 里残留的数据播完（不改变通道状态，避免尾音被切断） */
void audio_out_drain(void);

/* 立即静音：关闭通道再打开，用于「停止播放」 */
esp_err_t audio_out_stop(void);
