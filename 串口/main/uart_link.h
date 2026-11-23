/*
 * uart_link.h - UART0 <-> STM32 通信链路
 *
 * 关键点（硬约束）：
 *   - 使用 UART0，引脚由代码显式绑定：TX0 = GPIO58，RX0 = GPIO59（用户明确指定）
 *   - 115200 / 8N1 / 无流控
 *   - console 已在 sdkconfig 中改到 USB Serial/JTAG，UART0 不被日志占用
 */
#pragma once

#include <stddef.h>

#include "driver/uart.h"
#include "esp_err.h"

#define UART_LINK_PORT      UART_NUM_0
#define UART_LINK_TX_GPIO   58      /* ESP32 TX0 -> STM32 PA3 (RX) */
#define UART_LINK_RX_GPIO   59      /* ESP32 RX0 <- STM32 PA2 (TX) */
#define UART_LINK_BAUD_RATE 115200

/* 收到一整行（不含 \r\n）时的回调，运行在 uart_rx 任务上下文 */
typedef void (*uart_link_line_cb_t)(const char *line, void *ctx);

/* 初始化 UART0 并启动接收任务 */
esp_err_t uart_link_init(uart_link_line_cb_t cb, void *ctx);

/* 发送一行：自动追加 "\n" */
esp_err_t uart_link_send_line(const char *text);
