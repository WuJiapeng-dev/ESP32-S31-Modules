/*
 * mic_i2s.c - 麦克风采集实现（INMP441 / ES8311 二选一，见 mic_i2s.h）
 *
 * 两个后端的关键差异：
 *   INMP441  数字 MEMS 麦，直接吐 I2S，24bit 左对齐塞在 32bit 槽里，
 *            不需要 I2C、不需要 MCLK，L/R 脚决定数据进左槽还是右槽。
 *   ES8311   模拟麦 + codec 芯片，必须先经 I2C 配寄存器才会输出 I2S 数据，
 *            需要 MCLK（4.096MHz = 16kHz × 256），输出就是 16bit 单声道。
 *
 * 为什么 ES8311 那套不能手写寄存器：
 *   上电后要按 MCLK/LRCK 比值查 coeff_div 表算分频，再依次打开 ADC 偏置、
 *   麦克风 PGA、时钟。少写一个寄存器就是一片静音且无任何报错，
 *   所以直接复用官方 esp_codec_dev 组件。
 */
#include "mic_i2s.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#if MIC_BACKEND == MIC_BACKEND_ES8311
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#endif

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx;

/* ================================================================== */
/*                          后端 A：ES8311                             */
/* ================================================================== */
#if MIC_BACKEND == MIC_BACKEND_ES8311

static esp_codec_dev_handle_t  s_dev;
static i2c_master_bus_handle_t s_i2c_bus;

/* 只在初始化失败后调用：把整条总线扫一遍，把「芯片不在」和「配置不对」分开。
   注意不能放在 open 之前用——那时 MCLK 还没跑，ES8311 不应答 I2C，
   扫描结果必然是 0 个设备（这个是实测踩过的坑，别再挪回去）。 */
static void diag_scan_i2c(void)
{
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_i2c_bus, a, 20) == ESP_OK) {
            ESP_LOGW(TAG, "  I2C 设备应答: 0x%02X", a);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "I2C 全总线（0x08-0x77）无应答：ES8311 没上电、或 SDA/SCL 与板子实际"
                      "引脚不符、或这块板没有该芯片");
    } else {
        ESP_LOGW(TAG, "总线上共 %d 个设备，但仍未配通；重点查 SDA=GPIO%d / SCL=GPIO%d 是否接反",
                 found, MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO);
    }
}

esp_err_t mic_i2s_init(void)
{
    esp_err_t err;

    /* ---- 1) I2C 控制通道 ---- */
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

    /* 整条总线扫一遍：0x18 在=ES8311 正常；有别的地址=接的不是这块板；
       一个都没有=板子没有 codec。三种情况后续处置完全不同。 */
    ESP_LOGI(TAG, "I2C 总线就绪（SCL=GPIO%d SDA=GPIO%d）",
             MIC_I2C_SCL_GPIO, MIC_I2C_SDA_GPIO);

    /* ---- 2) I2S RX ---- */
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
            .mclk = MIC_I2S_MCLK_GPIO,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_ES_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_ES_DIN_GPIO,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 必须在这里就把 I2S 跑起来。
       es8311_codec_new() 内部会立刻读写 ES8311 的寄存器做芯片探测，而
       ES8311 在 MCLK（GPIO52）没有时钟输出时不会应答 I2C —— 实测表现是
       一连串 "I2C_If: Fail to write to dev 30" 然后 "ES8311: Open fail"。
       esp_codec_dev_open 内部的 enable 来得太晚（它在建 codec 之后），
       所以这一步必须手动提前。 */
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %s", esp_err_to_name(err));
        return err;
    }
    /* 等 MCLK 稳定。HZ=100 时 pdMS_TO_TICKS(20) = 2 tick = 20ms */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "I2S 已启动，MCLK=GPIO%d 开始输出，等 ES8311 应答",
             MIC_I2S_MCLK_GPIO);

    /* ---- 3) ES8311 ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = I2C_NUM_0,
        .addr           = ES8311_CODEC_DEFAULT_ADDR,   /* 0x30，组件内部 >>1 */
        .bus_handle     = s_i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (ctrl_if == NULL || gpio_if == NULL) {
        ESP_LOGE(TAG, "创建 codec 控制接口失败");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .digital_mic = false,
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
    /* 关键：open 的内部顺序是
         set_fmt → enable I2S（GPIO52 开始输出 MCLK）→ codec set_fs → codec enable
       ES8311 必须等 MCLK 跑起来才会在 I2C 上应答，所以这一步之前
       绝不能去 probe I2C，否则必然扫不到任何设备。 */
    err = esp_codec_dev_open(s_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %s —— 开始 I2C 诊断", esp_err_to_name(err));
        diag_scan_i2c();
        return err;
    }
    esp_codec_dev_set_in_gain(s_dev, 30.0f);   /* 与 xiaozhi 板级代码一致 */

    ESP_LOGI(TAG, "ES8311 就绪: SCL=GPIO%d SDA=GPIO%d MCLK=GPIO%d BCLK=GPIO%d "
                  "WS=GPIO%d DIN=GPIO%d @ %dHz 16bit 单声道",
             MIC_I2C_SCL_GPIO, MIC_I2C_SDA_GPIO, MIC_I2S_MCLK_GPIO,
             MIC_I2S_BCLK_GPIO, MIC_I2S_ES_WS_GPIO, MIC_I2S_ES_DIN_GPIO,
             MIC_SAMPLE_RATE);
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
        return 0;
    }
    return frames;
}

/* ================================================================== */
/*                    后端 C：PDM 数字麦（两根线）                      */
/* ================================================================== */
#elif MIC_BACKEND == MIC_BACKEND_PDM

#include "driver/i2s_pdm.h"

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

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_PDM_CLK_GPIO,
            .din = MIC_PDM_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "PDM 麦就绪: CLK=GPIO%d DATA=GPIO%d @ %dHz 16bit 单声道",
             MIC_PDM_CLK_GPIO, MIC_PDM_DATA_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    if (s_rx == NULL || dst == NULL || frames == 0) {
        return 0;
    }

    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, dst, frames * sizeof(int16_t),
                                     &bytes, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "i2s_channel_read 失败: %s", esp_err_to_name(err));
        return 0;
    }
    return bytes / sizeof(int16_t);
}

/* ================================================================== */
/*                         后端 B：INMP441                             */
/* ================================================================== */
#else   /* MIC_BACKEND == MIC_BACKEND_INMP441 */

static int32_t s_raw[256];

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
            .invert_flags = { false, false, false },
        },
    };
    /* L/R 接 VDD → 数据只在右槽位出现 */
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

    ESP_LOGI(TAG, "INMP441 就绪: SCK=GPIO%d WS=GPIO%d SD=GPIO%d @ %dHz "
                  "32bit槽/取右声道（L/R 必须接 3V3）",
             MIC_I2S_SCK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    size_t done = 0;

    if (s_rx == NULL || dst == NULL) {
        return 0;
    }

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
            break;      /* 超时，先把已读到的交出去 */
        }

        /* 24bit 数据在 32bit 槽里左对齐 → 右移 16 位取高 16bit */
        for (size_t i = 0; i < n; i++) {
            dst[done + i] = (int16_t)(s_raw[i] >> 16);
        }
        done += n;

        if (n < chunk) {
            break;
        }
    }

    return done;
}

#endif  /* MIC_BACKEND */
