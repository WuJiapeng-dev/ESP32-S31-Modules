/*
 * ESP32-S31 + SYN6288 中文语音合成（BLE -> UART -> TTS）
 *
 * 功能：手机 BLE 连上 "kskbl zdjd"，向写特征发送任意 UTF-8 文本，
 *       ESP32 转成 GB2312 后按 SYN6288 帧协议经 UART1 发给模块播报；
 *       播报状态经 Notify 特征回传（OK:PLAY / ACK:xx / OK:DONE / WARN:*）。
 *
 * 控制台：已配置为 USB Serial/JTAG（见 sdkconfig.defaults），
 *         UART1（GPIO9/8）专职与 SYN6288 通信，日志不会串到模块里。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_bridge.h"
#include "gb2312.h"
#include "syn6288.h"
#include "tts.h"

static const char *TAG = "laba";

/* IDF 的 NimBLE 端口不负责注册 store 回调，需要应用侧调用（与官方 bleprph 示例一致） */
void ble_store_config_init(void);

static void on_stack_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位，reason=%d，等待同步", reason);
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
    nimble_port_run();          /* 只在 nimble_port_stop() 后返回 */
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    /* 1. NVS（BLE 存 PHY 校准数据 / 绑定信息需要） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "==== ESP32-S31 SYN6288 语音合成模块启动 ====");
    ESP_LOGI(TAG, "接线: GPIO%d(TX)->RXD, GPIO%d(RX)<-TXD, GPIO%d<-BY, %d 8N1",
             SYN6288_TX_GPIO, SYN6288_RX_GPIO, SYN6288_BY_GPIO, SYN6288_BAUD_DEFAULT);

    /* 2. SYN6288 串口 + BY 引脚 */
    ESP_ERROR_CHECK(syn6288_init());
    ESP_LOGI(TAG, "GB2312 码表: %u 项", (unsigned)gb2312_table_size());

    /* 3. 播放任务（转码 + 分帧发送 + 等待播完，全在这里做，不阻塞 BLE） */
    ESP_ERROR_CHECK(tts_init());

    /* 4. NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 5. 注册 GATT 服务（必须在 host 任务启动前） */
    ESP_ERROR_CHECK(ble_bridge_init());

    /* 6. 注册 NVS 存储回调（配对/绑定信息落盘用；未配对时也要求存在） */
    ble_store_config_init();

    /* 7. 启动 host 任务，同步后自动开始广播 */
    nimble_port_freertos_init(nimble_host_task);
}
