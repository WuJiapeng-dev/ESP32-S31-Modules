/*
 * ESP32-S31 麦克风识别 → BLE 送手机显示（phone_recv）
 *
 * 链路：
 *   INMP441（I2S 从机）→ 每 10ms 取 160 样本 → MFCC → 端点检测
 *     → 与已教过的模板做 DTW 比对 → 命中则 BLE Notify 把文字推给手机
 *
 * 本工程**不接喇叭**：没有 UART1、没有 SYN6288、没有 GB2312 转码、没有播放任务。
 * 所有反馈（命令回执、识别结果）都走 Notify 特征 6a400003。
 *
 * 默认状态：识别关闭（避免上电就误触发），手机发 "ASR" 打开。
 * 使用顺序：PING → EN:你好 → 说"你好" → ASR → 再说"你好" → 手机显示 ASR:你好
 *
 * 控制台：UART0（GPIO58/59，板载 CH343），烧录线同时也是日志线。
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
#include "mic_i2s.h"

static const char *TAG = "board_mic";

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

    ESP_LOGI(TAG, "==== ESP32-S31 麦克风识别 → BLE（不接喇叭）====");
#if MIC_BACKEND == MIC_BACKEND_ES8389
    ESP_LOGI(TAG, "采集后端: 板载 ES8389（SDA=GPIO%d SCL=GPIO%d，"
                  "BCLK=GPIO%d WS=GPIO%d ASDOUT=GPIO%d，PA_CTRL=GPIO%d 拉低）",
             MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO, MIC_I2S_BCLK_GPIO,
             MIC_I2S_WS_GPIO, MIC_I2S_SD_GPIO, MIC_PA_CTRL_GPIO);
#else
    ESP_LOGI(TAG, "采集后端: 外接 INMP441（SCK=GPIO%d WS=GPIO%d SD=GPIO%d，"
                  "L/R 必须接 3V3）",
             MIC_I2S_SCK_GPIO, MIC_INMP441_WS_GPIO, MIC_INMP441_SD_GPIO);
#endif

    /* 2. 板载 ES8311 麦克风。
       故意不用 ESP_ERROR_CHECK：麦克风起不来时如果直接 panic，板子会进
       复位循环，手机连都连不上，反而什么都诊断不了。这里降级继续跑，
       BLE 仍可用 PING 自检，日志里也会写清是哪一步失败。 */
    esp_err_t mic_err = mic_i2s_init();
    if (mic_err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败(%s)：识别功能不可用，BLE 仍正常",
                 esp_err_to_name(mic_err));
    }

    /* 3. 识别（内部创建 asr 任务，并把 NVS 里的模板读回内存） */
    ESP_ERROR_CHECK(asr_init());

    ESP_LOGI(TAG, "启动完成，空闲堆 %u 字节", (unsigned)esp_get_free_heap_size());

    /* 4. NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb  = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 5. 注册 GATT 服务（必须在 host 任务启动前） */
    ESP_ERROR_CHECK(ble_bridge_init());

    /* 6. 注册 NVS 存储回调（配对/绑定信息落盘用；未配对时也要求存在） */
    ble_store_config_init();

    /* 7. 启动 host 任务，同步后自动开始广播 */
    nimble_port_freertos_init(nimble_host_task);
}
