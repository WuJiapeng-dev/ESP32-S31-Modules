/*
 * es8389.h - 慧勤智远 ESP32-S31 CB V1.1 板载音频编解码器 ES8389 精简驱动
 *
 * 只做一件事：把 ADC 录音链路打开（双路模拟 MEMS 麦克风 -> PGA1/PGA2 -> 双 ADC
 * -> I2S 从机输出），不涉及 DAC 放音。
 *
 * 寄存器序列来源（不是猜的）：
 *   esp_codec_dev 官方组件 1.6.2 的 device/es8389/es8389.c，
 *   es8389_open() / es8389_config_sample() / es8389_config_fmt() /
 *   es8389_set_bits_per_sample() / es8389_apply_mic_pga_gain() / es8389_set_bias_on()
 *   六个函数按调用顺序展开，把 update_bits() 的读改写结果预先算成定值。
 *
 * 本工程采用的配置（与 ESP-IDF 版固件完全一致，已实测跑通）：
 *   master_mode   = false   codec 作 I2S 从机，BCLK/LRCK 由 ESP32 提供
 *   use_mclk      = false   不驱动 MCLK，内部时钟由 BCLK 派生
 *                           （见 es8389.c: if (!codec->use_mclk) es8389_config_sample(...)）
 *   no_dac_ref    = true    只录音，不用 DAC 输出作参考，0x23 不加 0x80
 *   16bit / 16kHz / I2S 标准格式
 *   采样率系数按 no_dac_ref=true 取 mclk_fre = 16000*16*2 = 512000，
 *   查 coeff_div[] 得 {Ratio=32, MCLK=512000, LRCK=16000} 那一行
 */
#pragma once

#include <stdint.h>

/* ES8389 7bit I2C 地址（8bit 写地址 0x20 >> 1） */
#define ES8389_I2C_ADDR   0x10

/*
 * 初始化 I2C 并写完整条 ADC 链路。
 * sda/scl 为 GPIO 编号（板载为 0 / 1）。
 * 返回 true 表示器件应答且寄存器全部写入成功。
 */
bool es8389_begin(int sda, int scl);

/* 器件是否在 I2C 总线上应答（用于开机自检打印） */
bool es8389_probe(void);

/*
 * 设置两路麦克风 PGA 增益，单位 dB。
 * 表内档位：0 / 3.5 / 6.5 / 9.5 / 12.5 / 15.5 / 18.5 / 21.5 / 24.5 / 27.5
 *           / 30.5 / 33.5 / 36.5，不在表内则向下取最近档位。
 * 板载实测结论：30dB 时安静环境 RMS≈100、正常说话峰值 2700 左右，
 * 距满量程 32767 还有约 11dB 余量，所以默认 30dB。
 */
void es8389_set_mic_gain_db(float db);

/* 读一个寄存器（调试用，读失败返回 0xFF） */
uint8_t es8389_read_reg(uint8_t reg);

/* 写一个寄存器（调试用） */
bool es8389_write_reg(uint8_t reg, uint8_t val);
