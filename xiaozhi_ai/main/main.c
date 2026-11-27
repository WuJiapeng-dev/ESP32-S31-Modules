/*
 * main.c - ESP32-S31 语音对话（INMP441 + MAX98357 + DeepSeek ASR/LLM/TTS）
 *
 * 硬件（用户指定接线）：
 *   INMP441  SCK=GPIO19  WS=GPIO46  SD=GPIO13  L/R=VDD(右声道)
 *   MAX98357 BCLK=GPIO57 LRC=GPIO56 DIN=GPIO38 SD/GAIN=GPIO37
 *
 * 流程：
 *   开机 -> 起配网热点 kskbl-setup（APSTA 常驻）-> 若已有配置则连路由器
 *        -> 对话任务开始监听，说话即录音
 *        -> DeepSeek ASR 识别 -> DeepSeek LLM 回答 -> DeepSeek TTS 合成 -> MAX98357 播报
 *
 * 控制台在 USB Serial/JTAG 上，115200，中文 UTF-8。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "audio_out.h"
#include "cfg_store.h"
#include "dialog.h"
#include "mic_i2s.h"
#include "wifi_prov.h"

static const char *TAG = "app";

void app_main(void)
{
    /* 1. NVS：Wi-Fi 与 API Key 都存在这里 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "==== ESP32-S31 语音对话（小智ai接入）%s ====", APP_VER);
    ESP_LOGI(TAG, "采集：INMP441 SCK=GPIO%d WS=GPIO%d SD=GPIO%d，%dHz 16bit 单声道",
             MIC_I2S_SCK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO, MIC_SAMPLE_RATE);
    ESP_LOGI(TAG, "播放：%s @ %dHz", AUDIO_OUT_DESC, AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "云端：%s（ASR / LLM / TTS 三件套同一把 Key）", DS_API_BASE);

    /* 2. 音频输入输出 */
    ESP_ERROR_CHECK(mic_i2s_init());
    ESP_ERROR_CHECK(audio_out_init());

    /* 3. Wi-Fi：先起配网热点，保证任何情况下都能打开网页改配置 */
    ESP_ERROR_CHECK(wifi_core_init());
    ESP_ERROR_CHECK(wifi_prov_start());

    app_cfg_t cfg;
    cfg_set_defaults(&cfg);
    if (cfg_load(&cfg) == ESP_OK && cfg_ready(&cfg)) {
        ESP_LOGI(TAG, "已有配置：Wi-Fi=\"%s\"，音色=%s，尝试连接…",
                 cfg.wifi_ssid, cfg.voice[0] ? cfg.voice : CFG_VOICE_DEFAULT);
        wifi_sta_connect(cfg.wifi_ssid, cfg.wifi_pass);
    } else {
        ESP_LOGW(TAG, "配置不完整，请手机连热点 \"%s\"，浏览器打开 http://%s/ 填写",
                 PROV_AP_SSID, PROV_AP_IP);
    }

    /* 4. 对话任务 */
    ESP_ERROR_CHECK(dialog_start());

    ESP_LOGI(TAG, "就绪：直接对着板子说话即可（无需按键、无需唤醒词）");
    ESP_LOGI(TAG, "随时可打开 http://%s/ 修改 Wi-Fi / API Key / 音色", PROV_AP_IP);
}
