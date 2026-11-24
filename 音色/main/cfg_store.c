/*
 * cfg_store.c - NVS 配置读写
 *
 * 引擎两套凭据并存：切 engine 只是换「用哪一组」，另一组不会被清掉。
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cfg_store.h"

static const char *TAG = "cfg";

#define NVS_NS      "lcfg"
#define K_SSID      "ssid"
#define K_PASS      "pass"
#define K_ENGINE    "engine"
#define K_APIKEY    "apikey"
#define K_VOICE     "voice"
#define K_DBAPPID   "dbappid"
#define K_DBTOKEN   "dbtoken"
#define K_DBCLUSTER "dbcluster"
#define K_DBVOICE   "dbvoice"

void cfg_set_defaults(app_cfg_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    strlcpy(c->engine, CFG_ENGINE_DEFAULT, sizeof(c->engine));
    strlcpy(c->voice, CFG_VOICE_DEFAULT, sizeof(c->voice));
    strlcpy(c->db_cluster, CFG_DB_CLUSTER_DEFAULT, sizeof(c->db_cluster));
    strlcpy(c->db_voice, CFG_DB_VOICE_DEFAULT, sizeof(c->db_voice));
}

/* 统一读一个字符串键：不存在就保留默认值 */
static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读 %s 失败: %s", key, esp_err_to_name(err));
        dst[0] = '\0';
        return;
    }
    /* nvs_get_str 已保证以 0 结尾，这里再兜一层 */
    dst[cap - 1] = '\0';
}

esp_err_t cfg_load(app_cfg_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_set_defaults(c);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    read_str(h, K_SSID, c->wifi_ssid, sizeof(c->wifi_ssid));
    read_str(h, K_PASS, c->wifi_pass, sizeof(c->wifi_pass));
    read_str(h, K_ENGINE, c->engine, sizeof(c->engine));
    read_str(h, K_APIKEY, c->api_key, sizeof(c->api_key));
    read_str(h, K_VOICE, c->voice, sizeof(c->voice));
    read_str(h, K_DBAPPID, c->db_appid, sizeof(c->db_appid));
    read_str(h, K_DBTOKEN, c->db_token, sizeof(c->db_token));
    read_str(h, K_DBCLUSTER, c->db_cluster, sizeof(c->db_cluster));
    read_str(h, K_DBVOICE, c->db_voice, sizeof(c->db_voice));

    nvs_close(h);

    /* 引擎值不合法（老固件没这个键，或手改过 NVS）就回落 DeepSeek */
    if (!cfg_engine_valid(c->engine)) {
        ESP_LOGW(TAG, "引擎 \"%s\" 不合法，回落到 %s", c->engine, CFG_ENGINE_DEFAULT);
        strlcpy(c->engine, CFG_ENGINE_DEFAULT, sizeof(c->engine));
    }

    /* 老固件（火山版）存过 voice=BV700_streaming 之类，换引擎后不合法，回落 */
    if (!cfg_voice_valid(c->voice)) {
        ESP_LOGW(TAG, "音色 \"%s\" 不是 DeepSeek 的合法值，回落到 %s",
                 c->voice, CFG_VOICE_DEFAULT);
        strlcpy(c->voice, CFG_VOICE_DEFAULT, sizeof(c->voice));
    }

    ESP_LOGI(TAG, "配置: ssid=%s engine=%s ds_key=%s db_appid=%s db_token=%s",
             c->wifi_ssid[0] ? c->wifi_ssid : "(空)",
             c->engine,
             c->api_key[0] ? "(已设置)" : "(空)",
             c->db_appid[0] ? c->db_appid : "(空)",
             c->db_token[0] ? "(已设置)" : "(空)");
    return ESP_OK;
}

esp_err_t cfg_save(const app_cfg_t *c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* 任一字段写失败都直接返回，避免存半套配置 */
    err = nvs_set_str(h, K_SSID, c->wifi_ssid);
    if (err == ESP_OK) { err = nvs_set_str(h, K_PASS, c->wifi_pass); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_ENGINE, c->engine); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_APIKEY, c->api_key); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_VOICE, c->voice); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_DBAPPID, c->db_appid); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_DBTOKEN, c->db_token); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_DBCLUSTER, c->db_cluster); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_DBVOICE, c->db_voice); }
    if (err == ESP_OK) { err = nvs_commit(h); }

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool cfg_ready(const app_cfg_t *c)
{
    if (c == NULL || c->wifi_ssid[0] == '\0') {
        return false;
    }
    if (cfg_engine_is_doubao(c)) {
        /* 豆包：AppID + Access Token 两样必需，cluster / voice 有默认值兜底 */
        return c->db_appid[0] != '\0' && c->db_token[0] != '\0';
    }
    /* DeepSeek：一个 Key + 合法音色 */
    return c->api_key[0] != '\0' && cfg_voice_valid(c->voice);
}

bool cfg_voice_valid(const char *voice)
{
    if (voice == NULL) {
        return false;
    }
    return strcmp(voice, CFG_VOICE_STELLA) == 0
        || strcmp(voice, CFG_VOICE_TIDE) == 0;
}

bool cfg_engine_valid(const char *engine)
{
    if (engine == NULL) {
        return false;
    }
    return strcmp(engine, CFG_ENGINE_DS) == 0
        || strcmp(engine, CFG_ENGINE_DB) == 0;
}

esp_err_t cfg_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
