/*
 * ESP32-S31 麦克风语音复述（INMP441 -> 离线识别 -> BLE Notify + SYN6288 播报）
 *
 * 链路：
 *   INMP441（I2S 从机）→ ESP32 每 10ms 取 160 样本 → MFCC → 端点检测
 *     → 与已教过的模板做 DTW 比对 → 命中则
 *         ① BLE Notify 把文字推给手机显示（特征 6a400003）
 *         ② tts_say() 交给 SYN6288 念出来（UART1，9600 8N1，GB2312）
 *
 * 默认状态：识别是**关闭**的（避免上电就误触发），手机发 "ASR" 打开。
 * 先发 "EN:你好" 教一句，再发 "ASR"，然后说"你好"就能看到 Notify + 听到播报。
 *
 * 控制台：USB Serial/JTAG（见 sdkconfig.defaults），UART0 空闲，
 *         UART1（GPIO9/8）专职与 SYN6288 通信，日志不会串进模块。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "asr.h"
#include "ble_bridge.h"
#include "gb2312.h"
#include "mic_i2s.h"
#include "syn6288.h"
#include "tts.h"

static const char *TAG = "maikefeng";

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
    /* 1. NVS（BLE 校准/绑定 + 语音模板都存在这里） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "==== ESP32-S31 麦克风语音复述模块启动 ====");
    ESP_LOGI(TAG, "麦克风 INMP441: SCK=GPIO%d WS=GPIO%d SD=GPIO%d (L/R 接 VDD，取右声道)",
             MIC_I2S_SCK_GPIO, MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO);
    ESP_LOGI(TAG, "喇叭 SYN6288: TX=GPIO%d -> RXD, RX=GPIO%d <- TXD, BY=GPIO%d, %d 8N1",
             SYN6288_TX_GPIO, SYN6288_RX_GPIO, SYN6288_BY_GPIO, SYN6288_BAUD_DEFAULT);

    /* 2. SYN6288 串口 + BY 引脚 */
    ESP_ERROR_CHECK(syn6288_init());
    ESP_LOGI(TAG, "GB2312 码表: %u 项", (unsigned)gb2312_table_size());

    /* 3. 播放任务（转码 + 分帧发送 + 等播完，全在这里做，不阻塞其它任务） */
    ESP_ERROR_CHECK(tts_init());

    /* 4. 麦克风 I2S */
    ESP_ERROR_CHECK(mic_i2s_init());

    /* 5. 识别（内部创建 asr 任务，并把 NVS 里的模板读回内存） */
    ESP_ERROR_CHECK(asr_init());

    ESP_LOGI(TAG, "启动完成，空闲堆 %u 字节", (unsigned)esp_get_free_heap_size());

    /* 6. NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb  = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 7. 注册 GATT 服务（必须在 host 任务启动前） */
    ESP_ERROR_CHECK(ble_bridge_init());

    /* 8. 注册 NVS 存储回调（配对/绑定信息落盘用；未配对时也要求存在） */
    ble_store_config_init();

    /* 9. 启动 host 任务，同步后自动开始广播 */
    nimble_port_freertos_init(nimble_host_task);
}
