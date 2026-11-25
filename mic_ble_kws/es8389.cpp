/*
 * es8389.cpp - ES8389 录音链路初始化（精简移植自 esp_codec_dev 1.6.2）
 *
 * 每个寄存器块都标了对应驱动函数，便于对照核对。
 */
#include "es8389.h"

#include <Arduino.h>
#include <Wire.h>

#define ES8389_I2C_HZ 400000

bool es8389_write_reg(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(ES8389_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

uint8_t es8389_read_reg(uint8_t reg)
{
    Wire.beginTransmission(ES8389_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return 0xFF;
    }
    if (Wire.requestFrom((uint8_t)ES8389_I2C_ADDR, (uint8_t)1) != 1) {
        return 0xFF;
    }
    return (uint8_t)Wire.read();
}

bool es8389_probe(void)
{
    Wire.beginTransmission(ES8389_I2C_ADDR);
    return Wire.endTransmission() == 0;
}

/* 读改写：等价于驱动里的 es8389_update_bits() */
static bool upd(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v = es8389_read_reg(reg);
    if (v == 0xFF) {
        /* 读失败时不要瞎写，直接报错，避免把未知默认值写坏 */
        return false;
    }
    v = (uint8_t)((v & (uint8_t)~mask) | (val & mask));
    return es8389_write_reg(reg, v);
}

static bool wr_table(const uint8_t (*tbl)[2], size_t n)
{
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (!es8389_write_reg(tbl[i][0], tbl[i][1])) {
            Serial.printf("[ES8389] 写寄存器 0x%02X=0x%02X 失败\r\n", tbl[i][0], tbl[i][1]);
            ok = false;
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* es8389_open(): 上电总初始化                                         */
/* master_mode=false, use_mclk=false, no_dac_ref=true, 均不反相        */
/* 0x01(主从位) 与 0x02(时钟源) 的读改写结果已按上述参数算成定值       */
/* ------------------------------------------------------------------ */
static const uint8_t k_init_tbl[][2] = {
    {0xF3, 0x00},   /* 解除隔离 */
    {0x00, 0x7E},   /* 软复位 */
    {0xF3, 0x38},
    {0x24, 0x64},   /* ADC 控制 */
    {0x25, 0x04},
    {0x45, 0x03},   /* DAC 控制（本工程不用，按驱动原值写） */
    {0x60, 0x2A},   /* VMID */
    {0x61, 0xC9},   /* 模拟控制：MIC 偏置上电 */
    {0x62, 0x4F},
    {0x63, 0x06},
    {0x6B, 0x00},
    {0x6D, 0x16},
    {0x6E, 0xAA},
    {0x6F, 0x66},
    {0x70, 0x99},
    {0x23, 0x00},   /* no_dac_ref=true：不加内部参考信号 */
    {0x72, 0x10},   /* PGA1 增益初值：输入选择=1(差分 MIC1P1N)，增益 0dB */
    {0x73, 0x10},   /* PGA2 增益初值：输入选择=1(差分 MIC2N2P)，增益 0dB */
    {0x10, 0xC4},   /* 时钟管理：先给个安全值 */
    {0x01, 0x08},   /* MISC：MSModeSel=0 -> 从机 */
    {0xF1, 0x00},
    {0x12, 0x01},
    {0x13, 0x01},
    {0x14, 0x01},
    {0x15, 0x01},
    {0x16, 0x35},
    {0x17, 0x09},
    {0x18, 0x91},
    {0x19, 0x28},
    {0x1A, 0x01},
    {0x1B, 0x01},
    {0x1C, 0x11},
    {0x2A, 0x00},
    {0x20, 0x60},   /* ADC：16bit(3<<5) + I2S 格式(0<<2) */
    {0x40, 0x60},   /* DAC：同上 */
    {0xF0, 0x01},   /* 芯片杂项 */
    {0x02, 0x40},   /* 时钟源选择 = BCLK 派生（use_mclk=false 时写 1<<6） */
    {0x04, 0x00},   /* 分频初值，稍后由 config_sample 覆盖 */
    {0x05, 0x10},
    {0x06, 0x00},
    {0x07, 0xC0},
    {0x08, 0x00},
    {0x09, 0xC0},
    {0x0A, 0x80},
    {0x0B, 0x04},   /* MCLK 分频 256 的高位 */
    {0x0C, 0x01},   /* 256>>8 */
    {0x0D, 0x00},   /* 256&0xFF */
    {0x0F, 0x10},
    {0x21, 0x1F},
    {0x22, 0x7F},
    {0x2F, 0xC0},
    {0x30, 0xF4},
    {0x31, 0x00},
    {0x44, 0x00},
    {0x41, 0x7F},
    {0x42, 0x7F},
    {0x43, 0x10},
    {0x49, 0x0F},
    {0x4C, 0xC0},
    {0x00, 0x00},   /* 退出复位 */
    {0x03, 0xC1},   /* 电源管理 */
    {0x00, 0x01},
    {0x4D, 0x00},
    {0x26, 0xBF},   /* ADC 音量 */
    {0x27, 0xBF},
    {0x28, 0xBF},
    {0x46, 0xBF},   /* DAC 音量 */
    {0x47, 0xBF},
    {0x48, 0xBE},   /* 95<<1 */
};

/* ------------------------------------------------------------------ */
/* es8389_config_sample(): 16kHz / 16bit / coeff_div[32,512000,16000] */
/* 这一行在驱动表里是：{32, 512000, 16000, 0x00,0x55,0x84,0xD0,...}   */
/* use_mclk=false 时驱动只在这条分支里写分频寄存器，所以必须显式写    */
/* ------------------------------------------------------------------ */
static const uint8_t k_rate_tbl[][2] = {
    {0x04, 0x00},
    {0x05, 0x55},
    {0x06, 0x84},
    {0x07, 0xD0},
    {0x08, 0x01},
    {0x09, 0xC1},
    {0x0A, 0x90},
    {0x0F, 0x10},
    {0x11, 0x00},
    {0x21, 0x1F},
    {0x22, 0x7F},
    {0x26, 0xBF},
    {0x30, 0xF4},
    {0x41, 0xFF},
    {0x42, 0x7F},
    {0x43, 0x11},
    {0xF0, 0x12},
    {0xF1, 0x00},
    {0x16, 0x12},
    {0x18, 0x31},
    {0x19, 0x0E},
};

/* es8389_set_bias_on()：模拟偏置真正打开，必须放在分频配置之后 */
static const uint8_t k_bias_on_tbl[][2] = {
    {0x4D, 0x00},
    {0x61, 0xD9},
    {0x64, 0x8F},
    {0x10, 0xE4},
    {0x00, 0x01},
    {0x03, 0xC3},
    {0x24, 0x6A},
    {0x25, 0x0A},
};

/* 增益档位表：PGA 寄存器低 4 位的编码（es8389_mic_gain_t） */
static const struct {
    float   db;
    uint8_t code;
} k_gain_tbl[] = {
    {  0.0f, 0b0000}, {  3.5f, 0b0001}, {  6.5f, 0b0010}, {  9.5f, 0b0011},
    { 12.5f, 0b0110}, { 15.5f, 0b0111}, { 18.5f, 0b1000}, { 21.5f, 0b1001},
    { 24.5f, 0b1010}, { 27.5f, 0b1011}, { 30.5f, 0b1100}, { 33.5f, 0b1101},
    { 36.5f, 0b1110},
};

void es8389_set_mic_gain_db(float db)
{
    uint8_t code = 0;
    for (size_t i = 0; i < sizeof(k_gain_tbl) / sizeof(k_gain_tbl[0]); i++) {
        if (db >= k_gain_tbl[i].db) {
            code = k_gain_tbl[i].code;
        }
    }
    /* 输入选择 = 1（差分），放在 bit6..4 */
    uint8_t v = (uint8_t)((1 << 4) | code);
    es8389_write_reg(0x72, v);
    es8389_write_reg(0x73, v);
    Serial.printf("[ES8389] 麦克风 PGA 设为 %.1fdB（0x72=0x%02X）\r\n", (double)db, v);
}

bool es8389_begin(int sda, int scl)
{
    Wire.begin(sda, scl, ES8389_I2C_HZ);
    Wire.setTimeOut(50);

    if (!es8389_probe()) {
        Serial.printf("[ES8389] I2C 无应答（SDA=%d SCL=%d 7bit 0x%02X）\r\n",
                      sda, scl, ES8389_I2C_ADDR);
        return false;
    }
    Serial.printf("[ES8389] I2C 探测到器件（SDA=%d SCL=%d 7bit 0x%02X）\r\n",
                  sda, scl, ES8389_I2C_ADDR);

    bool ok = true;
    ok &= wr_table(k_init_tbl, sizeof(k_init_tbl) / sizeof(k_init_tbl[0]));
    ok &= wr_table(k_rate_tbl, sizeof(k_rate_tbl) / sizeof(k_rate_tbl[0]));

    /* 0x69 bit5：模拟偏置使能（驱动里是 update_bits，这里同样读改写） */
    ok &= upd(0x69, 0x20, 0x20);

    ok &= wr_table(k_bias_on_tbl, sizeof(k_bias_on_tbl) / sizeof(k_bias_on_tbl[0]));

    /* 关键寄存器回读，便于和手册/驱动对照 */
    Serial.printf("[ES8389] 回读 0x05=0x%02X 0x06=0x%02X 0x07=0x%02X 0x09=0x%02X 0x02=0x%02X\r\n",
                  es8389_read_reg(0x05), es8389_read_reg(0x06),
                  es8389_read_reg(0x07), es8389_read_reg(0x09),
                  es8389_read_reg(0x02));

    if (!ok) {
        Serial.println("[ES8389] 初始化过程中有寄存器写入失败");
    }
    return ok;
}
