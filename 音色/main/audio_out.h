/*
 * audio_out.h - I2S 数字音频输出（接 I2S 功放模块，如 MAX98357A / PCM5102）
 *
 * 接线（本模块默认，均在 soc_caps.h 确认有效的 GPIO 上）：
 *   ESP32 GPIO15 -> 功放 BCLK  (位时钟)
 *   ESP32 GPIO16 -> 功放 LRC   (帧时钟，也叫 WS / LRCLK)
 *   ESP32 GPIO17 -> 功放 DIN   (数据，进功放的 SD / DIN)
 *   3V3          -> 功放 VIN
 *   GND          -> 功放 GND
 *
 * 说明：
 *   - 语音链路只覆盖 GPIO15/16/17，避开 UART0(58/59)、USB PHY(33/34)、
 *     原 SYN6288 用的 GPIO8/9/12，隔壁喇叭模块的线不用拆。
 *   - DeepSeek TTS 返回单声道 16bit PCM；这里输出立体声并把同一份样本复制到
 *     L/R 两个 slot，兼容 MAX98357A 这类会把左右混合的功放。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_I2S_BCLK_GPIO     15
#define AUDIO_I2S_LRC_GPIO      16
#define AUDIO_I2S_DIN_GPIO      17

/* DeepSeek format=pcm 固定 24kHz、16bit、单声道，不可调 */
#define AUDIO_SAMPLE_RATE       24000

esp_err_t audio_out_init(void);

/* 写入单声道 16bit PCM（samples 为样本个数，不是字节数）。阻塞直到 DMA 吞吐完 */
esp_err_t audio_out_write_mono16(const int16_t *pcm, size_t samples);

/* 等 DMA 里残留的数据播完（不改变通道状态，避免尾音被切断） */
void audio_out_drain(void);

/* 立即静音：关闭通道再打开，用于「停止播放」 */
esp_err_t audio_out_stop(void);
