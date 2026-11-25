/*
 * mic_i2s.h - 板载 ES8311 麦克风采集（I2S 标准模式 / Philips / 16kHz）
 *
 * 为什么改成 ES8311：
 *   本机板型是 ESP32-S31-Function-CoreBoard-1，板载 ES8311 单声道 codec +
 *   板载麦克风（来源：xiaozhi-esp32 仓库 main/boards/espressif/
 *   esp32-s31-function-coreboard-1/README.md 与 config.h）。
 *   外接 INMP441 是不必要的——板子上本来就有麦克风，而且 ES8311 需要 I2C
 *   配置寄存器才会出数据，只接 I2S 是收不到任何信号的。
 *
 * 采样率定 16kHz：
 *   ES8311 的时钟分频表里有 {MCLK=4096000, rate=16000} 这一行
 *   （es8311.c 的 coeff_div），4.096MHz = 16kHz × 256，正好是最常用的比值，
 *   所以不需要重采样，MFCC/DTW 那一层一行都不用改。
 *
 * 引脚全部来自板级 config.h，注意 GPIO50–57/60/61 是板载外设专用，不要复用。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 板载 ES8311 / I2S（来源：板级 config.h） */
#define MIC_I2C_SCL_GPIO    50      /* ES8311 I2C SCL */
#define MIC_I2C_SDA_GPIO    51      /* ES8311 I2C SDA */
#define MIC_I2S_MCLK_GPIO   52      /* ES8311 MCLK  */
#define MIC_I2S_BCLK_GPIO   53      /* I2S BCLK / SCLK */
#define MIC_I2S_WS_GPIO     55      /* I2S WS / LRCK */
#define MIC_I2S_DIN_GPIO    54      /* ES8311 ASDOUT → ESP32（录音） */
#define MIC_I2S_DOUT_GPIO   56      /* ES8311 DSDIN ← ESP32（本工程不用） */

#define MIC_SAMPLE_RATE     16000

esp_err_t mic_i2s_init(void);

/*
 * 读单声道 16bit 样本。
 * 返回实际读到的样本数（正常应等于 frames）。
 */
size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms);
