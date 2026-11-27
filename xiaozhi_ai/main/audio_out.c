/*
 * audio_out.c - 音频输出实现
 *
 * 两条路径由 audio_out.h 的 AUDIO_OUT_BACKEND 决定：
 *   PDM 模式：写 PCM -> 硬件 PCM2PDM -> 单根引脚输出脉冲 -> 外部 RC 滤波
 *   I2S 模式：写 PCM -> I2S 标准模式三线输出 -> 数字功放
 * 对上层来说只有一个 audio_out_write_mono16()，改后端不影响业务代码。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
/* audio_out.h 必须先包含：AUDIO_OUT_BACKEND / AUDIO_BACKEND_PDM 都由它定义，
   否则下面的条件包含会把未定义宏当 0 处理，误包含 i2s_pdm.h。 */
#include "audio_out.h"
#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM
#include "driver/i2s_pdm.h"
#else
#include "driver/i2s_std.h"
#endif
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;

/* 中转块：256 样本 = 512 字节，栈上用不心疼 */
#define XFER_SAMPLES    256

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM
/* 衰减后的样本先放这里，不能直接改调用方的 buffer */
static int16_t s_pdm_buf[XFER_SAMPLES];
#endif

/* DMA 深度，两种模式共用，drain 的计算也依赖它 */
#define DMA_DESC_NUM    8
#define DMA_FRAME_NUM   240

esp_err_t audio_out_init(void)
{
    if (s_tx != NULL) {
        return ESP_OK;
    }

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_I2S
    /* MAX98357A 的 SD_MODE：高=使能，低=shutdown。
       先拉低再配 I2S，避免配置过程中喇叭"啪"一声。 */
    gpio_config_t sd_cfg = {
        .pin_bit_mask = 1ULL << AUDIO_I2S_SD_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd_cfg), TAG, "功放 SD 引脚配置失败");
    gpio_set_level((gpio_num_t)AUDIO_I2S_SD_GPIO, 0);
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 语音是持续流式写入，DMA 多留几帧，减少网络抖动导致的断音 */
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;   /* 无数据时输出 0，而不是循环旧数据 */

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "新建 I2S 通道失败");

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM
    /*
     * DAC 线模式（ONE_LINE_DAC）：单根数据线输出 PDM，专门给「RC 滤波当 DAC 用」的场景。
     * data_fmt = PCM 表示我们写的是普通 PCM，硬件 PCM2PDM 负责调制，
     * 所以上层完全不用关心 PDM 编码。
     */
    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk  = AUDIO_PDM_CLK_GPIO,
            .dout = AUDIO_PDM_DATA_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_tx_mode(s_tx, &pdm_cfg), TAG, "PDM TX 初始化失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "PDM 通道使能失败");

    ESP_LOGI(TAG, "PDM 就绪: DOUT=GPIO%d CLK=GPIO%d(不接线) @ %dHz, 数字衰减 >>%d",
             AUDIO_PDM_DATA_GPIO, AUDIO_PDM_CLK_GPIO, AUDIO_SAMPLE_RATE, AUDIO_PDM_ATTEN_SHIFT);
#else
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

    /* I2S 起来之后再打开功放，第一帧数据不会丢 */
    gpio_set_level((gpio_num_t)AUDIO_I2S_SD_GPIO, 1);
    ESP_LOGI(TAG, "MAX98357A 使能: SD(GPIO%d)=高", AUDIO_I2S_SD_GPIO);
#endif

    return ESP_OK;
}

esp_err_t audio_out_set_enable(bool on)
{
#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_I2S
    gpio_set_level((gpio_num_t)AUDIO_I2S_SD_GPIO, on ? 1 : 0);
    return ESP_OK;
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t audio_out_write_mono16(const int16_t *pcm, size_t samples)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0) {
        return ESP_OK;
    }

    while (samples > 0) {
        size_t n = (samples > XFER_SAMPLES) ? XFER_SAMPLES : samples;

#if AUDIO_OUT_BACKEND == AUDIO_BACKEND_PDM
        /*
         * 单声道 PDM：不复制成双声道，直接写 n 个 int16。
         * 右移做数字衰减（GCC 对负数算术右移，保留符号），
         * 目的是把 3.3Vpp 摆幅压到 PAM8403 24dB 增益能接受的范围。
         */
        for (size_t i = 0; i < n; i++) {
            s_pdm_buf[i] = (int16_t)(pcm[i] >> AUDIO_PDM_ATTEN_SHIFT);
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_pdm_buf, n * sizeof(int16_t),
                                          &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PDM 写入失败: %s", esp_err_to_name(err));
            return err;
        }
#else
        int16_t stereo[XFER_SAMPLES * 2];
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
#endif

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
     * i2s_channel_write 返回时数据只是进了 DMA。按 DMA 深度算残留时长：
     * dma_desc_num * dma_frame_num / sample_rate = 8 * 240 / 24000 = 80 ms。
     * 不去 disable 通道，避免把最后一个字切断。
     */
    uint32_t ms = (uint32_t)((DMA_DESC_NUM * DMA_FRAME_NUM * 1000ULL) / AUDIO_SAMPLE_RATE);
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
