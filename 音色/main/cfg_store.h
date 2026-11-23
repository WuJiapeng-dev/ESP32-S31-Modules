/*
 * cfg_store.h - 配置持久化（NVS）
 *
 * 存的东西全部来自手机配网页面，编译期不写死任何密钥：
 *   Wi-Fi SSID / 密码
 *   火山引擎 AppID / Access Token（控制台 → 语音合成 → 应用管理）
 *   cluster（标准音色固定 volcano_tts）
 *   voice_type（音色，例如 BV700_streaming）
 *   speed_ratio / volume_ratio（浮点 0.1~2.0 / 0.1~3.0，存储时 ×100 取整）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 字段长度按实际上限 + 1 留足，配网页提交超长一律截断 */
#define CFG_SSID_MAX        33      /* 802.11 SSID 最长 32 字节 */
#define CFG_PASS_MAX        65      /* WPA2 口令最长 64 字节 */
#define CFG_APPID_MAX       40
#define CFG_TOKEN_MAX       160
#define CFG_CLUSTER_MAX     32
#define CFG_VOICE_MAX       64

/* 火山标准音色集群，非大模型音色 */
#define CFG_CLUSTER_DEFAULT "volcano_tts"
/* 火山默认音色（标准版通用女声），可在配网页改 */
#define CFG_VOICE_DEFAULT   "BV700_streaming"

typedef struct {
    char     wifi_ssid[CFG_SSID_MAX];
    char     wifi_pass[CFG_PASS_MAX];
    char     appid[CFG_APPID_MAX];
    char     token[CFG_TOKEN_MAX];
    char     cluster[CFG_CLUSTER_MAX];
    char     voice[CFG_VOICE_MAX];
    uint16_t speed_x100;        /* 100 = 1.0 倍速 */
    uint16_t volume_x100;       /* 100 = 1.0 倍音量 */
} app_cfg_t;

/* 全部置零 + 填默认值（cluster / voice / speed / volume） */
void cfg_set_defaults(app_cfg_t *c);

/* 从 NVS 读；缺失字段填默认值。返回 ESP_ERR_NVS_NOT_FOUND 表示从未配置过 */
esp_err_t cfg_load(app_cfg_t *c);

/* 全量写入并提交 */
esp_err_t cfg_save(const app_cfg_t *c);

/* Wi-Fi + 密钥都齐了才算可工作 */
bool cfg_ready(const app_cfg_t *c);

/* 擦除全部配置（配网页「恢复出厂」用） */
esp_err_t cfg_erase(void);
