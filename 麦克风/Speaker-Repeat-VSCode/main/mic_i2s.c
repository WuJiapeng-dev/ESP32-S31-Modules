/*
 * mic_i2s.c - INMP441 采集实现
 *
 * 关键配置（每条都有原因，改之前先看注释）：
 *   1) 32bit 槽位：INMP441 输出 24bit 数据，左对齐塞在 32bit 槽里，
 *      用 16bit 槽位会丢掉低 8 位精度（能用但不划算），用 32bit 直接右移 16 位取高 16bit。
 *   2) slot_mask = RIGHT：模块 L/R 脚接 VDD 表示"右声道输出"，
 *      所以只取右槽位的数据，避免左右交错、变成两倍速的怪声音。
 *   3) Philips 格式（msb_right = false）：INMP441 手册标注的标准 I2S 时序。
 *   4) DMA 6 × 240 帧：约 6 × 15ms ≈ 90ms 缓冲，USB/BLE 抖动下不丢样本。
 */
#include "mic_i2s.h"

#include <string.h>

#include "esp_log.h"
#include "driver/i2s_std.h"

static const char *TAG = "mic";

static i2s_chan_handle_t s_rx;
static int32_t           s_raw[256];

esp_err_t mic_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* INMP441 不需要 MCLK */
            .bclk = MIC_I2S_SCK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /* L/R 接 VDD → 数据出现在右槽位 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S RX 就绪: SCK=GPIO%d WS=GPIO%d SD=GPIO%d @ %dHz 32bit槽/取右声道",
             MIC_I2S_SCK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    size_t done = 0;

    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > (sizeof(s_raw) / sizeof(s_raw[0]))) {
            chunk = sizeof(s_raw) / sizeof(s_raw[0]);
        }

        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_raw, chunk * sizeof(int32_t),
                                         &bytes, timeout_ms);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "i2s_channel_read 失败: %s", esp_err_to_name(err));
            break;
        }

        size_t n = bytes / sizeof(int32_t);
        if (n == 0) {
            break;
        }

        /* 24bit 数据在 32bit 槽里左对齐 → 右移 16 位得到 16bit 样本 */
        for (size_t i = 0; i < n; i++) {
            dst[done + i] = (int16_t)(s_raw[i] >> 16);
        }
        done += n;

        if (n < chunk) {
            break;      /* 超时，先把已读到的交出去 */
        }
    }

    return done;
}
