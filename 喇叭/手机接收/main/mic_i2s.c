/*
 * mic_i2s.c - 板载 ES8311 采集实现
 *
 * 三层结构（顺序不能换）：
 *   1) I2C 总线：ES8311 是靠寄存器配置的，没有 I2C 就只是个哑巴芯片
 *   2) I2S RX 通道：ESP32 做主机，出 MCLK/BCLK/WS，从 DIN 收数据
 *   3) esp_codec_dev：把 ES8311 寄存器初始化 + 读取封装起来
 *
 * 为什么必须经过 esp_codec_dev：
 *   ES8311 上电是关着的，要先按 MCLK/LRCK 比值算出分频（coeff_div 表）、
 *   再依次打开 ADC 偏置、麦克风 PGA、时钟。少写一个寄存器就是一片静音，
 *   而且没有任何报错。这里直接用官方驱动，不手写寄存器表。
 *
 * 为什么 16bit 而不是 32bit 槽：
 *   ES8311 的 I2S 输出就是 16bit（板级代码里 bits_per_sample = 16），
 *   和 INMP441 的 24bit 左对齐不是一回事，不需要移位。
 */
#include "mic_i2s.h"

#include <string.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "mic";

static i2s_chan_handle_t       s_rx;
static esp_codec_dev_handle_t  s_dev;
static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t mic_i2s_init(void)
{
    esp_err_t err;

    /* ---------- 1) I2C：ES8311 控制通道 ---------- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = MIC_I2C_SDA_GPIO,
        .scl_io_num        = MIC_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---------- 2) I2S RX：只收不发，MCLK 必须给 ---------- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MIC_I2S_MCLK_GPIO,      /* ES8311 必须有 MCLK */
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,        /* 本工程不播放 */
            .din  = MIC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* 16kHz × 256 = 4.096MHz，正好命中 coeff_div 表里的一行 */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---------- 3) ES8311 codec ---------- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = I2C_NUM_0,
        .addr           = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle     = s_i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl 失败");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio 失败");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,  /* 只开 ADC（录音） */
        .pa_pin      = -1,                            /* 本工程不驱动功放 */
        .pa_reverted = false,
        .master_mode = false,                         /* ESP32 出时钟，ES8311 从机 */
        .use_mclk    = true,
        .digital_mic = false,                         /* 板载是模拟麦进 ES8311 */
        .mclk_div    = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new 失败");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t data_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data 失败");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new 失败");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .channel_mask    = 0,
        .sample_rate     = MIC_SAMPLE_RATE,
        .mclk_multiple   = 256,
    };
    /* open 内部会打开 ES8311 并 enable I2S 通道，不需要手动 enable */
    err = esp_codec_dev_open(s_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %s（检查 ES8311 是否焊在板上、I2C 50/51）",
                 esp_err_to_name(err));
        return err;
    }

    esp_codec_dev_set_in_gain(s_dev, 30.0f);   /* 与 xiaozhi 板级代码一致 */

    ESP_LOGI(TAG, "ES8311 就绪: I2C SCL=GPIO%d SDA=GPIO%d, I2S MCLK=GPIO%d BCLK=GPIO%d "
                  "WS=GPIO%d DIN=GPIO%d @ %dHz 16bit 单声道",
             MIC_I2C_SCL_GPIO, MIC_I2C_SDA_GPIO, MIC_I2S_MCLK_GPIO,
             MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_DIN_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (s_dev == NULL || dst == NULL || frames == 0) {
        return 0;
    }

    int ret = esp_codec_dev_read(s_dev, dst, (int)(frames * sizeof(int16_t)));
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_read 失败: %d", ret);
        return 0;
    }
    return frames;
}
