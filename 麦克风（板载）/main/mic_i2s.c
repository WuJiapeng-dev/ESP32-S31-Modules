/*
 * mic_i2s.c - 麦克风采集实现（ES8389 板载 / INMP441 外接，编译期二选一）
 *
 * 默认后端 = 本板（慧勤智远 ESP32-S31 CB V1.1）的板载麦克风：
 *   MIC1/MIC2 (LMA3729T381) ──模拟──► ES8389 ADC ──I2S ASDOUT──► ESP32
 *
 * ES8389 的时钟谁给：本方案让 codec 自己从 BCLK 派生内部时钟
 * （use_mclk = false，对应厂商例程 19_music 的做法）。
 * 原因不是偷懒，而是 esp_codec_dev 的 es8389 驱动里：
 *     if (!codec->use_mclk) es8389_config_sample(...);
 * 一旦置 use_mclk = true，它压根不写 0x04~0x0A、0x21、0x22 这些分频寄存器，
 * 16kHz 就落不到表里，表现为能开但采样率不对。
 * 走 BCLK 派生时它按 BCLK/LRCK 比值查表：16bit × 2 声道 = 比值 32，
 * 表里正好有 {Ratio=32, MCLK=512000, LRCK=16000}，即 BCLK=512kHz。
 */
#include "mic_i2s.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#if MIC_BACKEND == MIC_BACKEND_ES8389
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8389_codec.h"
#endif

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx;

/* ================================================================== */
/*                     后端 A：ES8389（本板板载）                        */
/* ================================================================== */
#if MIC_BACKEND == MIC_BACKEND_ES8389

#define ES8389_7BIT_ADDR (ES8389_CODEC_DEFAULT_ADDR >> 1)   /* 0x10 */

static esp_codec_dev_handle_t  s_dev;
static i2c_master_bus_handle_t s_i2c_bus;

/* 只在初始化失败后调用：把整条总线扫一遍，把「芯片不在」和「配置不对」分开。
   这条总线上还挂着触摸 GT1151Q(0x14) 和摄像头，所以扫到别的地址是正常的。 */
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
        ESP_LOGE(TAG, "I2C 全总线（0x08-0x77）无应答：检查 SDA=GPIO%d / SCL=GPIO%d 是否接反、"
                      "或 AUDIO_3V3 没起来", MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO);
    } else {
        ESP_LOGW(TAG, "总线上共 %d 个设备；ES8389 应在 0x%02X，若不在说明没上电或地址不对",
                 found, ES8389_7BIT_ADDR);
    }
}

/* 开完 codec 立刻量一次电平：这是「麦克风到底有没有在出数据」的直接证据。
   有声音时 RMS 是几十~上千；一片静音时 RMS=0、峰值=0。 */
static void diag_audio_level(void)
{
    enum { FRAMES = 200, ROUNDS = 10 };     /* 2000 帧 = 125ms @16k */
    static int16_t buf[FRAMES * 2];
    int64_t acc_l = 0, acc_r = 0;
    int32_t pk_l = 0, pk_r = 0;
    int n = 0;

    for (int k = 0; k < ROUNDS; k++) {
        if (esp_codec_dev_read(s_dev, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "电平自检读失败，跳过");
            return;
        }
        for (int i = 0; i < FRAMES; i++) {
            int16_t l = buf[2 * i];
            int16_t r = buf[2 * i + 1];
            acc_l += (int32_t)l * l;
            acc_r += (int32_t)r * r;
            int32_t al = l < 0 ? -l : l;
            int32_t ar = r < 0 ? -r : r;
            if (al > pk_l) pk_l = al;
            if (ar > pk_r) pk_r = ar;
        }
        n += FRAMES;
    }
    int rms_l = (int)sqrtf((float)acc_l / n);
    int rms_r = (int)sqrtf((float)acc_r / n);
    ESP_LOGI(TAG, "电平自检: 左 RMS=%d 峰值=%d | 右 RMS=%d 峰值=%d（全 0 = 没有数据）",
             rms_l, pk_l, rms_r, pk_r);
    if (pk_l == 0 && pk_r == 0) {
        ESP_LOGE(TAG, "两路都是 0：codec 已配通但没有音频数据，先查 ASDOUT(GPIO%d) 与 BCLK/WS",
                 MIC_I2S_SD_GPIO);
    }
}

esp_err_t mic_i2s_init(void)
{
    esp_err_t err;

    /* ---- 0) 功放使能拉低：录音时不需要喇叭，避免底噪串进来 ---- */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << MIC_PA_CTRL_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(MIC_PA_CTRL_GPIO, 0);

    /* ---- 1) I2C 控制通道（与触摸/摄像头共用，板上有 4.7k 上拉）---- */
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

    err = i2c_master_probe(s_i2c_bus, ES8389_7BIT_ADDR, 100);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C 探测到 ES8389（SDA=GPIO%d SCL=GPIO%d，7bit 0x%02X）",
                 MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO, ES8389_7BIT_ADDR);
    } else {
        ESP_LOGE(TAG, "I2C 0x%02X 无应答（%s）——继续尝试初始化，失败会打印总线扫描",
                 ES8389_7BIT_ADDR, esp_err_to_name(err));
    }

    /* ---- 2) I2S RX：16kHz / 16bit / 双声道 ----
       BCLK 由 ESP32 输出 = 16000 × 16 × 2 = 512kHz，正好命中 codec 系数表。
       不驱动 MCLK（GPIO2 留空），codec 从 BCLK 派生内部时钟。 */
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
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,        /* codec 从 BCLK 取时钟 */
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws   = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_I2S_SD_GPIO,        /* ASDOUT：codec → ESP32 */
            .invert_flags = { false, false, false },
        },
    };
    /* 双声道都收：MIC1/MIC2 分别落在左/右，mic_i2s_read 里再挑一路 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

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
    vTaskDelay(pdMS_TO_TICKS(20));      /* 让 BCLK/WS 稳定下来再配置 codec */
    ESP_LOGI(TAG, "I2S 已启动: BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d，BCLK=512kHz",
             MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO);

    /* ---- 3) ES8389 ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port           = I2C_NUM_0,
        .addr           = ES8389_CODEC_DEFAULT_ADDR,   /* 0x20，组件内部 >>1 成 0x10 */
        .bus_handle     = s_i2c_bus,
        .clock_speed_hz = 400000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (ctrl_if == NULL || gpio_if == NULL) {
        ESP_LOGE(TAG, "创建 codec 控制接口失败");
        return ESP_FAIL;
    }

    es8389_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,   /* 只开录音通路 */
        .pa_pin      = -1,                            /* 功放自己拉低，不让组件碰 */
        .pa_reverted = false,
        .master_mode = false,                         /* codec 作 I2S 从机 */
        .use_mclk    = false,                         /* 时钟从 BCLK 派生，见文件头 */
        .digital_mic = false,                         /* 板载的是模拟麦 */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f, .pa_gain = 0 },
        .no_dac_ref  = true,                          /* 录单声道，不用 DAC 当参考 */
        .mclk_div    = 256,
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "es8389_codec_new 失败（芯片 ID 读不到）");
        diag_scan_i2c();
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
        .channel         = 2,           /* 双声道：BCLK/LRCK 比值 32 */
        .channel_mask    = 0,
        .sample_rate     = MIC_SAMPLE_RATE,
        .mclk_multiple   = 256,         /* 未使用 MCLK，仅为字段完整 */
    };
    err = esp_codec_dev_open(s_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %s —— 开始 I2C 诊断", esp_err_to_name(err));
        diag_scan_i2c();
        return err;
    }

    err = esp_codec_dev_set_in_gain(s_dev, MIC_PGA_DB);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置麦克风增益 %.1fdB 失败: %s（继续）", (double)MIC_PGA_DB,
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ES8389 就绪: SDA=GPIO%d SCL=GPIO%d BCLK=GPIO%d WS=GPIO%d "
                  "ASDOUT=GPIO%d @ %dHz 16bit 双声道（PGA %.1fdB，取%s声道）",
             MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO, MIC_I2S_BCLK_GPIO, MIC_I2S_WS_GPIO,
             MIC_I2S_SD_GPIO, MIC_SAMPLE_RATE, (double)MIC_PGA_DB,
             MIC_CH_SEL == 0 ? "左" : (MIC_CH_SEL == 1 ? "右" : "平均两"));

    diag_audio_level();
    return ESP_OK;
}

size_t mic_i2s_read(int16_t *dst, size_t frames, uint32_t timeout_ms)
{
    enum { MAX_FRAMES = 1024 };
    static int16_t st[MAX_FRAMES * 2];

    (void)timeout_ms;
    if (s_dev == NULL || dst == NULL || frames == 0) {
        return 0;
    }
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }

    int ret = esp_codec_dev_read(s_dev, st, (int)(frames * 2 * sizeof(int16_t)));
    if (ret != ESP_CODEC_DEV_OK) {
        return 0;
    }

    if (MIC_CH_SEL == 2) {
        for (size_t i = 0; i < frames; i++) {
            dst[i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
        }
    } else {
        const size_t off = (MIC_CH_SEL == 1) ? 1 : 0;
        for (size_t i = 0; i < frames; i++) {
            dst[i] = st[2 * i + off];
        }
    }
    return frames;
}

/* ================================================================== */
/*                       后端 B：INMP441（外接）                        */
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
            .ws   = MIC_INMP441_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_INMP441_SD_GPIO,
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
             MIC_I2S_SCK_GPIO, MIC_INMP441_WS_GPIO, MIC_INMP441_SD_GPIO, MIC_SAMPLE_RATE);
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
