/*
 * audio_out.c - I2S 标准模式 TX，单声道样本复制成双声道输出
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "audio_out.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;

/* 单声道 -> 双声道的中转块：256 样本 => 512 个 int16 => 1KB，栈上用不心疼 */
#define XFER_SAMPLES    256

esp_err_t audio_out_init(void)
{
    if (s_tx != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 语音是持续流式写入，DMA 多留几帧，减少网络抖动导致的断音 */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear    = true;   /* 无数据时输出 0，而不是循环旧数据 */

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "新建 I2S 通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* MAX98357A / PCM5102 都不需要 MCLK */
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws   = AUDIO_I2S_LRC_GPIO,
            .dout = AUDIO_I2S_DIN_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "I2S std 模式初始化失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "I2S 通道使能失败");

    ESP_LOGI(TAG, "I2S 就绪: BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d, %d Hz 16bit 立体声",
             AUDIO_I2S_BCLK_GPIO, AUDIO_I2S_LRC_GPIO, AUDIO_I2S_DIN_GPIO, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_out_write_mono16(const int16_t *pcm, size_t samples)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0) {
        return ESP_OK;
    }

    int16_t stereo[XFER_SAMPLES * 2];

    while (samples > 0) {
        size_t n = (samples > XFER_SAMPLES) ? XFER_SAMPLES : samples;
        for (size_t i = 0; i < n; i++) {
            stereo[i * 2]     = pcm[i];
            stereo[i * 2 + 1] = pcm[i];
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, stereo, n * 2 * sizeof(int16_t),
                                          &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S 写入失败: %s", esp_err_to_name(err));
            return err;
        }
        pcm     += n;
        samples -= n;
    }
    return ESP_OK;
}

void audio_out_drain(void)
{
    if (s_tx == NULL) {
        return;
    }
    /*
     * i2s_channel_write 返回时数据只是进了 DMA。按 DMA 深度估算残留时长：
     * dma_desc_num * dma_frame_num / sample_rate = 8 * 240 / 16000 = 120 ms。
     * 不去 disable 通道，避免把最后一个字切断。
     */
    uint32_t ms = (uint32_t)(8000ULL * 240 * 8 / (AUDIO_SAMPLE_RATE * 1000));
    if (ms < 60) {
        ms = 60;
    }
    vTaskDelay(pdMS_TO_TICKS(ms + 40));
}

esp_err_t audio_out_stop(void)
{
    if (s_tx == NULL) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_tx);
}
