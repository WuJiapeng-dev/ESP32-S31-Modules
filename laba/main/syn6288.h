/*
 * syn6288.h - SYN6288 中文语音合成模块驱动（UART + BY 忙信号）
 *
 * 接线（用户指定，逐字采用）：
 *   ESP32 GPIO09 (TX) -> SYN6288 RXD
 *   ESP32 GPIO08 (RX) <- SYN6288 TXD
 *   ESP32 GPIO12      <- SYN6288 BY（忙信号：高=正在合成播放，低=空闲可接收）
 *   3V3 -> VCC , GND -> GND
 *
 * 协议来源：《SYN6288E 中文语音合成芯片 用户手册》（宇音天下）
 *           http://www.doc.voicetx.com/home/SYN6288E
 *   帧格式 = 帧头 0xFD + 数据区长度(2 字节，高字节在前) + 命令字 + 命令参数 + 文本 + 异或校验
 *   数据区长度 = 命令字(1) + 命令参数(1，无参数命令则为 0) + 文本(n) + 校验(1)
 *   异或校验   = 从帧头 0xFD 起到文本最后一个字节，全部按字节异或
 *
 *   手册给出的原始示例（本驱动的帧构造与之一一对应，可用来自检）：
 *     播放 GB2312 "宇音天下": FD 00 0B 01 00 D3 EE D2 F4 CC EC CF C2 C1
 *     播放 GBK    "宇音天下": FD 00 0B 01 01 D3 EE D2 F4 CC EC CF C2 C0
 *     设置 9600bps         : FD 00 03 31 00 CF
 *     停止合成             : FD 00 02 02 FD
 *     状态查询             : FD 00 02 21 DE
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ------------------------------ 硬件参数 ------------------------------ */
#define SYN6288_UART_NUM        1       /* UART1；U1 无默认 IOMUX 脚，必须显式 set_pin */
#define SYN6288_TX_GPIO         9       /* ESP32 TX -> 模块 RXD */
#define SYN6288_RX_GPIO         8       /* ESP32 RX <- 模块 TXD */
#define SYN6288_BY_GPIO         12      /* BY 忙信号输入 */

#define SYN6288_BAUD_DEFAULT    9600    /* 出厂默认 9600 8N1（手册 3.2 节） */

/* ------------------------------ 协议上限 ------------------------------ */
#define SYN6288_MAX_TEXT_BYTES  200     /* 手册：单帧待发送文本 <= 200 字节 */
#define SYN6288_FRAME_MAX       (1 + 2 + 1 + 1 + SYN6288_MAX_TEXT_BYTES + 1)

/* ------------------------------ 命令字 -------------------------------- */
#define SYN6288_CMD_PLAY        0x01    /* 语音合成播放 */
#define SYN6288_CMD_STOP        0x02    /* 停止合成 */
#define SYN6288_CMD_PAUSE       0x03    /* 暂停合成 */
#define SYN6288_CMD_RESUME      0x04    /* 恢复合成 */
#define SYN6288_CMD_QUERY       0x21    /* 状态查询：0x4E 合成中 / 0x4F 空闲 */
#define SYN6288_CMD_SET_BAUD    0x31    /* 设置波特率 */

/* 命令参数（播放命令）：低 3 位为文本编码，高 5 位为背景音乐编号（0=无） */
#define SYN6288_ENC_GB2312      0x00
#define SYN6288_ENC_GBK         0x01
#define SYN6288_ENC_BIG5        0x02
#define SYN6288_ENC_UNICODE     0x03

/* ------------------------------ 对外接口 ------------------------------ */
esp_err_t syn6288_init(void);

/* 构造并发送一帧“播放”命令；长文本会自动按 200 字节分帧并等上一帧播完 */
esp_err_t syn6288_play_gb2312(const uint8_t *gb, size_t len, size_t *frame_count);

/* 停止 / 暂停 / 恢复 */
esp_err_t syn6288_stop(void);

/* 切换模块波特率：code 0=9600 / 1=19200 / 2=38400（手册 4.2 节 0x31 命令） */
esp_err_t syn6288_set_baud_rate(uint8_t code);

/* BY 引脚：true = 芯片忙（正在合成播放），false = 空闲 */
bool syn6288_busy(void);

/* 等待 BY 变忙 / 变空闲，返回是否在超时内达到目标状态 */
bool syn6288_wait_busy(uint32_t timeout_ms);
bool syn6288_wait_idle(uint32_t timeout_ms);

/* 读取模块回传字节（0x41=命令正确，0x45=命令错误，0x4E/0x4F=状态），非阻塞式收集 */
size_t syn6288_read_reply(uint8_t *buf, size_t cap, uint32_t timeout_ms);
