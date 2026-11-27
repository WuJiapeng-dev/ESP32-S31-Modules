/*
 * cfg_store.h - 配置持久化（NVS）
 *
 * 存的东西全部来自控制台页面，编译期不写死任何密钥。
 *
 * 引擎可切换，两套凭据**同时保存**，切来切去不用重填：
 *   engine = "deepseek"  → 用 api_key + voice（stella / tide）
 *   engine = "doubao"    → 用 db_appid + db_token + db_cluster + db_voice
 *
 * 两个引擎的鉴权完全不同，这是并存而不是抽象成同一套的原因：
 *   DeepSeek : Authorization: Bearer <key>        （空格）
 *   豆包 V1  : Authorization: Bearer; <token>     （分号，容易踩坑）
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
#define CFG_ENGINE_MAX      12
#define CFG_APPID_MAX       32      /* 火山 AppID 是数字串 */
#define CFG_TOKEN_MAX       128
#define CFG_CLUSTER_MAX     32
#define CFG_DBVOICE_MAX     64      /* voice_type 如 zh_female_vv_uranus_bigtts */

/* ---- 引擎标识 ---- */
#define CFG_ENGINE_DS       "deepseek"
#define CFG_ENGINE_DB       "doubao"
#define CFG_ENGINE_DEFAULT  CFG_ENGINE_DS

/* ---- DeepSeek 音色：官方只有两个发音人 ---- */
#define CFG_VOICE_STELLA    "stella"    /* 海星，女声，默认 */
#define CFG_VOICE_TIDE      "tide"      /* 暗潮，男声 */
#define CFG_VOICE_DEFAULT   CFG_VOICE_STELLA

/* ---- 豆包默认值（标准音色集群）---- */
#define CFG_DB_CLUSTER_DEFAULT  "volcano_tts"
#define CFG_DB_VOICE_DEFAULT    "BV700_streaming"

typedef struct {
    char wifi_ssid[CFG_SSID_MAX];
    char wifi_pass[CFG_PASS_MAX];

    char engine[CFG_ENGINE_MAX];        /* 当前使用哪个引擎 */

    /* DeepSeek */
    char api_key[CFG_KEY_MAX];
    char voice[CFG_VOICE_MAX];

    /* 豆包 / 火山引擎 */
    char db_appid[CFG_APPID_MAX];
    char db_token[CFG_TOKEN_MAX];
    char db_cluster[CFG_CLUSTER_MAX];
    char db_voice[CFG_DBVOICE_MAX];
} app_cfg_t;

/* 全部置零 + 填默认值（engine / voice / cluster / db_voice） */
void cfg_set_defaults(app_cfg_t *c);

/* 从 NVS 读；缺失字段填默认值。返回 ESP_ERR_NVS_NOT_FOUND 表示从未配置过 */
esp_err_t cfg_load(app_cfg_t *c);

/* 全量写入并提交 */
esp_err_t cfg_save(const app_cfg_t *c);

/* Wi-Fi + 当前引擎的凭据都齐了才算可工作 */
bool cfg_ready(const app_cfg_t *c);

/* 擦除全部配置（控制台「恢复出厂」用） */
esp_err_t cfg_erase(void);

/* voice 是否合法（stella / tide），不合法时由调用方回落到默认 */
bool cfg_voice_valid(const char *voice);

/* engine 是否合法（deepseek / doubao） */
bool cfg_engine_valid(const char *engine);

/* 当前引擎是不是豆包 */
static inline bool cfg_engine_is_doubao(const app_cfg_t *c)
{
    /* 不引 string.h，比较前两个字节："doubao"=do，"deepseek"=de，够区分 */
    return c != NULL && c->engine[0] == 'd' && c->engine[1] == 'o';
}
