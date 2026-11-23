/*
 * uart_link.c - UART0 驱动封装（显式引脚 + 行解析）
 */
#include "uart_link.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart_link";

#define UART_RX_BUF_SIZE 1024
#define UART_TX_BUF_SIZE 256
#define UART_RX_TASK_STACK 3072
#define LINE_MAX_LEN 64

static uart_link_line_cb_t s_line_cb;
static void *s_line_cb_ctx;

/* 接收任务：把字节流按 \r 或 \n 切行后回调，不阻塞业务逻辑 */
static void uart_rx_task(void *arg)
{
    (void)arg;

    uint8_t chunk[128];
    char line[LINE_MAX_LEN];
    size_t len = 0;

    ESP_LOGI(TAG, "UART0 接收任务启动 (TX=%d RX=%d, %d 8N1)",
             UART_LINK_TX_GPIO, UART_LINK_RX_GPIO, UART_LINK_BAUD_RATE);

    for (;;) {
        int n = uart_read_bytes(UART_LINK_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    if (s_line_cb != NULL) {
                        s_line_cb(line, s_line_cb_ctx);
                    }
                    len = 0;
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                /* 超长行：整行丢弃，避免缓冲区溢出 */
                len = 0;
            }
        }
    }
}

esp_err_t uart_link_init(uart_link_line_cb_t cb, void *ctx)
{
    s_line_cb = cb;
    s_line_cb_ctx = ctx;

    const uart_config_t cfg = {
        .baud_rate  = UART_LINK_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_LINK_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 显式绑定引脚，不依赖芯片默认脚 */
    err = uart_set_pin(UART_LINK_PORT,
                       UART_LINK_TX_GPIO,
                       UART_LINK_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 关掉 UART 的默认日志驱动挂载（console 已改 USB Serial/JTAG，这里只是保险） */
    err = uart_driver_install(UART_LINK_PORT, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(uart_rx_task, "uart_rx", UART_RX_TASK_STACK, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 uart_rx 任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t uart_link_send_line(const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(text);
    if (len == 0 || len > LINE_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(UART_LINK_PORT, text, len);
    if (written != (int)len) {
        ESP_LOGE(TAG, "UART 写入正文失败 (%d/%u)", written, (unsigned)len);
        return ESP_FAIL;
    }

    written = uart_write_bytes(UART_LINK_PORT, "\n", 1);
    if (written != 1) {
        ESP_LOGE(TAG, "UART 写入换行失败 (%d/1)", written);
        return ESP_FAIL;
    }

    /* 等数据真正移位出去，保证 STM32 侧的时序可预期 */
    return uart_wait_tx_done(UART_LINK_PORT, pdMS_TO_TICKS(50));
}
