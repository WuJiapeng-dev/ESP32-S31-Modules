/*
 * ESP32-S31 云端语音合成（BLE 收文本 -> DeepSeek TTS -> I2S 功放）
 *
 * 链路：
 *   手机 BLE 写 "kskbl zdjd" 的写特征
 *     -> speak_feed() 累积、按标点切块
 *     -> ds_tts_speak() HTTPS 调 DeepSeek /v1/tools/tts，拿回 24kHz 裸 PCM
 *     -> audio_out_write_mono16() 推 I2S
 *     -> MAX98357A / PCM5102 类数字功放 -> 喇叭
 *
 * 配置：
 *   热点 kskbl-setup 常开，手机连上打开 http://192.168.4.1/ 填 Wi-Fi 和 DeepSeek API Key。
 *   APSTA 常驻，所以连上路由器后热点依然在，随时能改配置或试听。
 *
 * 控制台：USB Serial/JTAG（UART0 完全让位，留给串口模块的桥接用途）
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "audio_out.h"
#include "ble_bridge.h"
#include "cfg_store.h"
#include "speak.h"
#include "wifi_prov.h"

static const char *TAG = "yinse";

/* IDF 的 NimBLE 端口不注册 store 回调，需要应用侧调用（与官方 bleprph 示例一致） */
void ble_store_config_init(void);

static void on_stack_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位，reason=%d", reason);
}

static void on_stack_sync(void)
{
    ESP_LOGI(TAG, "NimBLE 同步完成");
    ble_bridge_start();
}

static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host 任务启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    /* 1. NVS：BLE 校准数据 + 本模块的配置都存在这里 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "==== ESP32-S31 云端语音合成模块启动 ====");
    ESP_LOGI(TAG, "I2S: BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d @ %dHz",
             AUDIO_I2S_BCLK_GPIO, AUDIO_I2S_LRC_GPIO, AUDIO_I2S_DIN_GPIO,
             AUDIO_SAMPLE_RATE);

    /* 2. 音频输出 */
    ESP_ERROR_CHECK(audio_out_init());

    /* 3. Wi-Fi：先起常开热点 + 控制台，再尝试连路由器
     *    这样即使路由器没配好，用户也能立刻打开 192.168.4.1 改配置，不会失联 */
    ESP_ERROR_CHECK(wifi_core_init());
    ESP_ERROR_CHECK(wifi_prov_start());

    app_cfg_t cfg;
    if (cfg_load(&cfg) == ESP_OK && cfg_ready(&cfg)) {
        ESP_LOGI(TAG, "已有完整配置，尝试连接 %s", cfg.wifi_ssid);
        wifi_sta_connect(cfg.wifi_ssid, cfg.wifi_pass);
        /* 不在这里死等：连上与否由后台事件决定，BLE 和播放任务先起来 */
    } else {
        ESP_LOGW(TAG, "配置不完整，请连热点 %s 打开 http://%s/ 配网",
                 PROV_AP_SSID, PROV_AP_IP);
    }

    /* 4. 播放调度任务 */
    ESP_ERROR_CHECK(speak_init());

    /* 5. NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb        = on_stack_reset;
    ble_hs_cfg.sync_cb         = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ESP_ERROR_CHECK(ble_bridge_init());
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "就绪：BLE \"kskbl zdjd\" / 控制台 http://%s/", PROV_AP_IP);
}
