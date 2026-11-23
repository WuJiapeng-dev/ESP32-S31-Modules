/*
 * ESP32-S31 BLE <-> UART0 桥
 *
 * 功能：
 *   BLE 手机写 "ON"  -> UART0 发 "ON\n"  -> STM32 PA1 置高 -> LED 亮
 *   BLE 手机写 "OFF" -> UART0 发 "OFF\n" -> STM32 PA1 置低 -> LED 灭
 *   执行结果经 Notify 特征回传（"OK:ON" / "OK:OFF"）
 *
 * 控制台：已配置为 USB Serial/JTAG（见 sdkconfig.defaults），UART0 专用于 STM32
 */
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_bridge.h"
#include "uart_link.h"

static const char *TAG = "kskbl";

/* IDF 的 NimBLE 端口不负责注册 store 回调，需要应用侧调用（与官方 bleprph 示例一致） */
void ble_store_config_init(void);

/* STM32 -> ESP32 的整行数据，仅用于调试观察（本工程不要求回传） */
static void on_stm32_line(const char *line, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "STM32 -> ESP32: \"%s\"", line);
}

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

    ESP_LOGI(TAG, "==== ESP32-S31 BLE(UART 桥) 启动 ====");
    ESP_LOGI(TAG, "UART0: TX=GPIO%d -> STM32 PA3, RX=GPIO%d <- STM32 PA2, %d 8N1",
             UART_LINK_TX_GPIO, UART_LINK_RX_GPIO, UART_LINK_BAUD_RATE);

    /* 2. UART0：先起链路，保证 BLE 一连接就能立刻下发指令 */
    ESP_ERROR_CHECK(uart_link_init(on_stm32_line, NULL));

    /* 3. NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 4. 注册 GATT 服务（必须在 host 任务启动前） */
    ESP_ERROR_CHECK(ble_bridge_init());

    /* 5. 注册 NVS 存储回调（配对/绑定信息落盘用；未配对时也要求存在） */
    ble_store_config_init();

    /* 6. 启动 host 任务，同步后自动开始广播 */
    nimble_port_freertos_init(nimble_host_task);
}
