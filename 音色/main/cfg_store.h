/*
 * cfg_store.h - 配置持久化（NVS）
 *
 * 存的东西全部来自控制台页面，编译期不写死任何密钥：
 *   Wi-Fi SSID / 密码
 *   DeepSeek API Key（形如 sk-xxxxxxxx，平台 → API keys 创建）
 *   voice（音色，只有 stella / tide 两个合法值）
 *
 * 与火山那版的差别：DeepSeek 只需要一个 Key，不需要 AppID / Token / cluster；
 * 也不支持语速、音量调节，所以那两个字段一并去掉了。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 字段长度按实际上限 + 1 留足，配网页提交超长一律截断 */
#define CFG_SSID_MAX        33      /* 802.11 SSID 最长 32 字节 */
#define CFG_PASS_MAX        65      /* WPA2 口令最长 64 字节 */
#define CFG_KEY_MAX         128     /* DeepSeek key 约 40~60 字符，留足余量 */
#define CFG_VOICE_MAX       16

/* 官方只有两个发音人 */
#define CFG_VOICE_STELLA    "stella"    /* 海星，女声，默认 */
#define CFG_VOICE_TIDE      "tide"      /* 暗潮，男声 */
#define CFG_VOICE_DEFAULT   CFG_VOICE_STELLA

typedef struct {
    char wifi_ssid[CFG_SSID_MAX];
    char wifi_pass[CFG_PASS_MAX];
    char api_key[CFG_KEY_MAX];
    char voice[CFG_VOICE_MAX];
} app_cfg_t;

/* 全部置零 + 填默认值（voice） */
void cfg_set_defaults(app_cfg_t *c);

/* 从 NVS 读；缺失字段填默认值。返回 ESP_ERR_NVS_NOT_FOUND 表示从未配置过 */
esp_err_t cfg_load(app_cfg_t *c);

/* 全量写入并提交 */
esp_err_t cfg_save(const app_cfg_t *c);

/* Wi-Fi + API Key 都齐了才算可工作 */
bool cfg_ready(const app_cfg_t *c);

/* 擦除全部配置（控制台「恢复出厂」用） */
esp_err_t cfg_erase(void);

/* voice 是否合法（stella / tide），不合法时由调用方回落到默认 */
bool cfg_voice_valid(const char *voice);
