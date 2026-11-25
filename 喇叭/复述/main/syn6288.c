/*
 * syn6288.c - SYN6288 驱动实现
 *
 * 所有帧格式与常量均对照《SYN6288E 用户手册》实现，
 * 关键示例已写在 syn6288.h 头部，便于逐字节核对。
 */
#include "syn6288.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "syn6288";

static uint32_t s_baud = SYN6288_BAUD_DEFAULT;

/*
 * 构造一帧并整帧一次性写入。
 * 分多次 uart_write_bytes 会在字节之间插入调度间隙（手册要求帧内字节间隔 < 8ms），
 * 所以这里先在栈上拼好整帧，再一次性交给驱动。
 */
static esp_err_t write_frame(uint8_t cmd, bool has_para, uint8_t para,
                             const uint8_t *text, size_t len)
{
    if (len > SYN6288_MAX_TEXT_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[SYN6288_FRAME_MAX];
    size_t n = 0;

    /* 数据区长度 = 命令字(1) + 命令参数(0或1) + 文本(len) + 校验(1)
     * 手册 4.1：高字节在前、低字节在后，长度必须与数据区实际长度严格一致。
     * 最高位没有特殊含义 —— 手册 4.4 的示例就是 0x00（如 FD 00 0B ...）。 */
    uint16_t data_len = (uint16_t)(1 + (has_para ? 1 : 0) + len + 1);

    frame[n++] = 0xFD;
    frame[n++] = (uint8_t)(data_len >> 8);
    frame[n++] = (uint8_t)(data_len & 0xFF);
    frame[n++] = cmd;
    if (has_para) {
        frame[n++] = para;
    }
    if (len > 0) {
        memcpy(&frame[n], text, len);
        n += len;
    }

    /* 异或校验：从帧头 0xFD 到文本最后一个字节，逐字节异或 */
    uint8_t xor_sum = 0;
    for (size_t i = 0; i < n; i++) {
        xor_sum ^= frame[i];
    }
    frame[n++] = xor_sum;

    int written = uart_write_bytes(SYN6288_UART_NUM, frame, n);
    if (written != (int)n) {
        ESP_LOGE(TAG, "UART 写入不足 (%d/%u)", written, (unsigned)n);
        return ESP_FAIL;
    }

    /* 等数据真正移位出去（9600bps 下 206 字节约 215ms，给 3s 余量） */
    return uart_wait_tx_done(SYN6288_UART_NUM, pdMS_TO_TICKS(3000));
}

/* 把长度对齐到 GB2312 字符边界：不切断双字节汉字 */
static size_t gb_char_align(const uint8_t *p, size_t n)
{
    size_t i = 0;

    while (i < n) {
        if (p[i] < 0x80) {
            i += 1;             /* ASCII 单字节 */
        } else if (i + 1 < n) {
            i += 2;             /* GB2312 双字节 */
        } else {
            break;              /* 末尾只剩一个首字节：留给下一帧 */
        }
    }
    return i;
}

esp_err_t syn6288_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = SYN6288_BAUD_DEFAULT,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(SYN6288_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 显式绑定引脚，不依赖芯片默认脚（U1/U2/U3 在 S31 上默认 IOMUX 为 -1） */
    err = uart_set_pin(SYN6288_UART_NUM, SYN6288_TX_GPIO, SYN6288_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(SYN6288_UART_NUM, 2048, 1024, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* BY 忙信号输入。
     * 用下拉而非上拉：模块没接时读到 0（空闲），这样“发送后等不到忙信号”
     * 能可靠地判成接线问题并回报 WARN:NOBY，而不是一直误判为忙。 */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << SYN6288_BY_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BY 引脚配置失败: %s", esp_err_to_name(err));
        return err;
    }

    s_baud = SYN6288_BAUD_DEFAULT;

    ESP_LOGI(TAG, "初始化完成: UART%d TX=GPIO%d RX=GPIO%d BY=GPIO%d %u 8N1",
             SYN6288_UART_NUM, SYN6288_TX_GPIO, SYN6288_RX_GPIO,
             SYN6288_BY_GPIO, (unsigned)s_baud);
    ESP_LOGI(TAG, "BY 当前电平=%d（0=空闲，1=忙）", gpio_get_level(SYN6288_BY_GPIO));

    return ESP_OK;
}

esp_err_t syn6288_play_gb2312(const uint8_t *gb, size_t len, size_t *frame_count)
{
    if (gb == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;
    size_t frames = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > SYN6288_MAX_TEXT_BYTES) {
            chunk = SYN6288_MAX_TEXT_BYTES;
        }
        chunk = gb_char_align(gb + off, chunk);
        if (chunk == 0) {
            break;
        }

        if (frames > 0) {
            /* 手册 4.3：芯片正在合成时收到新帧会立即打断当前合成，
             * 所以分帧播放必须等上一帧播完，否则前一段会被截断。 */
            if (!syn6288_wait_idle(60000)) {
                ESP_LOGW(TAG, "等待上一帧播完超时，继续发送");
            }
            vTaskDelay(pdMS_TO_TICKS(2));   /* 手册建议空闲后再延时约 1ms */
        }

        esp_err_t err = write_frame(SYN6288_CMD_PLAY, true, SYN6288_ENC_GB2312,
                                    gb + off, chunk);
        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGI(TAG, "已发送第 %u 帧，文本 %u 字节",
                 (unsigned)(frames + 1), (unsigned)chunk);

        off += chunk;
        frames++;

        if (off < len) {
            vTaskDelay(pdMS_TO_TICKS(10));  /* 手册：帧与帧之间必须 > 8ms */
        }
    }

    if (frame_count != NULL) {
        *frame_count = frames;
    }
    return ESP_OK;
}

esp_err_t syn6288_stop(void)
{
    /* 手册 4.4 示例：FD 00 02 02 FD —— 无命令参数字节 */
    return write_frame(SYN6288_CMD_STOP, false, 0, NULL, 0);
}

esp_err_t syn6288_set_baud_rate(uint8_t code)
{
    if (code > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 必须先用“当前波特率”把设置帧发出去，再改本地波特率 */
    esp_err_t err = write_frame(SYN6288_CMD_SET_BAUD, true, code, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    /* 手册 4.3：唤醒/改速后需间隔 16ms 才能发下一条，这里取 20ms 留余量 */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint32_t baud = (code == 0) ? 9600 : (code == 1) ? 19200 : 38400;
    err = uart_set_baudrate(SYN6288_UART_NUM, baud);
    if (err == ESP_OK) {
        s_baud = baud;
        ESP_LOGI(TAG, "模块与本地波特率已切换为 %u", (unsigned)baud);
    }
    return err;
}

bool syn6288_busy(void)
{
    return gpio_get_level(SYN6288_BY_GPIO) != 0;
}

bool syn6288_wait_busy(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (syn6288_busy()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return syn6288_busy();
}

bool syn6288_wait_idle(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (!syn6288_busy()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !syn6288_busy();
}

size_t syn6288_read_reply(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }

    size_t n = 0;
    TickType_t start = xTaskGetTickCount();

    while (n < cap && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        int r = uart_read_bytes(SYN6288_UART_NUM, buf + n, cap - n, pdMS_TO_TICKS(10));
        if (r > 0) {
            n += (size_t)r;
        }
    }
    return n;
}
