/*
 * mic_i2s.cpp - I2S 主机端采集
 *
 * 用 ESP-IDF 原生 i2s_std 驱动而不是 Arduino 的 ESP_I2S 封装，原因：
 *   本方案要求 codec 作从机、ESP32 只输出 BCLK/LRCK 且不输出 MCLK，
 *   并且要精确控制 slot 位宽与声道掩码，原生 API 更直接、出错时错误码也更清楚。
 */
#include "mic_i2s.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

#include "es8389.h"

static i2s_chan_handle_t s_rx = NULL;

bool mic_begin(void)
{
    /* 先关功放：两颗 NS4150B 的使能脚，录音期间必须为低 */
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << MIC_PA_CTRL_GPIO;
    pa.mode         = GPIO_MODE_OUTPUT;
    pa.pull_up_en   = GPIO_PULLUP_DISABLE;
    pa.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pa.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)MIC_PA_CTRL_GPIO, 0);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 6;
    cc.dma_frame_num = 240;

    esp_err_t err = i2s_new_channel(&cc, NULL, &s_rx);
    if (err != ESP_OK) {
        Serial.printf("[I2S] i2s_new_channel 失败: %s\r\n", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;                          /* 不驱动 MCLK */
    sc.gpio_cfg.bclk = (gpio_num_t)MIC_I2S_BCLK_GPIO;
    sc.gpio_cfg.ws   = (gpio_num_t)MIC_I2S_WS_GPIO;
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din  = (gpio_num_t)MIC_I2S_DIN_GPIO;

    err = i2s_channel_init_std_mode(s_rx, &sc);
    if (err != ESP_OK) {
        Serial.printf("[I2S] init_std_mode 失败: %s\r\n", esp_err_to_name(err));
        return false;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        Serial.printf("[I2S] channel_enable 失败: %s\r\n", esp_err_to_name(err));
        return false;
    }

    /* 16kHz × 16bit × 2 声道 = BCLK 512kHz，与 ES8389 分频表 ratio=32 对应 */
    Serial.printf("[I2S] 已启动：BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d，"
                  "%dHz 16bit 立体声，BCLK=512kHz，MCLK 不驱动\r\n",
                  MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_DIN_GPIO, MIC_SAMPLE_RATE);
    return true;
}

size_t mic_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    enum { MAX_FRAMES = 1024 };
    static int16_t st[MAX_FRAMES * 2];

    if (s_rx == NULL || dst == NULL || frames == 0) {
        return 0;
    }
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }

    size_t bytes = 0;
    TickType_t to = pdMS_TO_TICKS(timeout_ms);
    if (to == 0) {
        to = 1;
    }

    esp_err_t err = i2s_channel_read(s_rx, st, frames * 2 * sizeof(int16_t), &bytes, to);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        Serial.printf("[I2S] read 失败: %s\r\n", esp_err_to_name(err));
        return 0;
    }

    size_t got = bytes / (2 * sizeof(int16_t));
    if (got == 0) {
        return 0;
    }

    if (MIC_CH_SEL == 2) {
        for (size_t i = 0; i < got; i++) {
            dst[i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
        }
    } else {
        const size_t off = (MIC_CH_SEL == 1) ? 1 : 0;
        for (size_t i = 0; i < got; i++) {
            dst[i] = st[2 * i + off];
        }
    }
    return got;
}

void mic_level_check(void)
{
    enum { N = 400 };                       /* 40 帧 × 10ms = 400ms 数据 */
    static int16_t buf[N * 2];
    size_t bytes = 0;

    if (s_rx == NULL) {
        return;
    }
    if (i2s_channel_read(s_rx, buf, sizeof(buf), &bytes, pdMS_TO_TICKS(1000)) != ESP_OK) {
        Serial.println("[I2S] 电平自检读超时");
        return;
    }

    size_t n = bytes / (2 * sizeof(int16_t));
    double accL = 0, accR = 0;
    int    pkL = 0, pkR = 0;

    for (size_t i = 0; i < n; i++) {
        int l = buf[2 * i];
        int r = buf[2 * i + 1];
        accL += (double)l * l;
        accR += (double)r * r;
        if (abs(l) > pkL) pkL = abs(l);
        if (abs(r) > pkR) pkR = abs(r);
    }
    if (n == 0) {
        return;
    }

    Serial.printf("[I2S] 电平自检：左 RMS=%.0f 峰值=%d | 右 RMS=%.0f 峰值=%d"
                  "（RMS 接近 0 说明没采到数据）\r\n",
                  sqrt(accL / (double)n), pkL, sqrt(accR / (double)n), pkR);
}
