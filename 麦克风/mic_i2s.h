/*
 * mic_i2s.h - 板载麦克风采集（ES8389 + I2S 标准模式）
 *
 * 硬件路径（慧勤智远 ESP32-S31 CB V1.1 原理图 / 用户手册表 1.1）：
 *   两颗 LMA3729T381-OAS02 模拟 MEMS 麦克风
 *     -> ES8389 的 PGA1 / PGA2
 *     -> ES8389 双路 ADC
 *     -> ES8389 作 I2S 从机，从 ASDOUT 把 16bit 立体声交出来
 *     -> ESP32-S31 作 I2S 主机，BCLK/LRCK 由它输出
 *
 * 引脚沿用厂商原理图（与 ESP-IDF 版板载麦固件完全一致，已实测出数据）：
 *   I2C  SDA=GPIO0  SCL=GPIO1
 *   I2S  BCLK=GPIO3  WS(LRCK)=GPIO4  DIN(ASDOUT)=GPIO6
 *   PA   GPIO7  —— 两颗 NS4150B 功放使能，高电平开。录音时必须拉低，
 *                  否则喇叭底噪会串进麦克风
 *   MCLK GPIO2  —— 厂商 `I2S_MCK_IO = GPIO_NUM_NC`，本方案不驱动，
 *                  时钟由 BCLK 派生（见 es8389.cpp 头注释）
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MIC_SAMPLE_RATE     16000
#define MIC_I2C_SDA_GPIO    0
#define MIC_I2C_SCL_GPIO    1
#define MIC_I2S_BCLK_GPIO   3
#define MIC_I2S_WS_GPIO     4
#define MIC_I2S_DIN_GPIO    6
#define MIC_PA_CTRL_GPIO    7

/* 0=取左声道 1=取右声道 2=两路平均。板载两颗麦都接到了 ADC，取左即可 */
#define MIC_CH_SEL          0

/* 麦克风 PGA 增益。实测 30dB 时安静环境 RMS≈100、说话峰值约 2700，
   距满量程 32767 还有约 11dB 余量，再高容易在大声时削顶 */
#define MIC_PGA_DB          30.0f

/* 返回 true 表示 I2S 通道已启动（codec 是否配置成功另见 es8389_begin 的返回值） */
bool mic_begin(void);

/* 读 frames 个单声道样本（16bit），返回实际读到的个数，超时返回 0 */
size_t mic_read(int16_t *dst, size_t frames, uint32_t timeout_ms);

/* 一次性的电平自检：打印左右声道 RMS 与峰值，用来确认采集链路通 */
void mic_level_check(void);
