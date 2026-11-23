/*
 * cfg_store.c - NVS 配置读写
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
#define K_APPID     "appid"
#define K_TOKEN     "token"
#define K_CLUSTER   "cluster"
#define K_VOICE     "voice"
#define K_SPEED     "speed"
#define K_VOLUME    "volume"

void cfg_set_defaults(app_cfg_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    strlcpy(c->cluster, CFG_CLUSTER_DEFAULT, sizeof(c->cluster));
    strlcpy(c->voice, CFG_VOICE_DEFAULT, sizeof(c->voice));
    c->speed_x100  = 100;
    c->volume_x100 = 100;
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

static void read_u16(nvs_handle_t h, const char *key, uint16_t *dst)
{
    uint16_t v = 0;
    if (nvs_get_u16(h, key, &v) == ESP_OK && v > 0) {
        *dst = v;
    }
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
    read_str(h, K_APPID, c->appid, sizeof(c->appid));
    read_str(h, K_TOKEN, c->token, sizeof(c->token));
    read_str(h, K_CLUSTER, c->cluster, sizeof(c->cluster));
    read_str(h, K_VOICE, c->voice, sizeof(c->voice));
    read_u16(h, K_SPEED, &c->speed_x100);
    read_u16(h, K_VOLUME, &c->volume_x100);

    nvs_close(h);

    ESP_LOGI(TAG, "配置: ssid=%s appid=%s voice=%s speed=%u vol=%u",
             c->wifi_ssid[0] ? c->wifi_ssid : "(空)",
             c->appid[0] ? c->appid : "(空)",
             c->voice, c->speed_x100, c->volume_x100);
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
    if (err == ESP_OK) { err = nvs_set_str(h, K_APPID, c->appid); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_TOKEN, c->token); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_CLUSTER, c->cluster); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_VOICE, c->voice); }
    if (err == ESP_OK) { err = nvs_set_u16(h, K_SPEED, c->speed_x100); }
    if (err == ESP_OK) { err = nvs_set_u16(h, K_VOLUME, c->volume_x100); }
    if (err == ESP_OK) { err = nvs_commit(h); }

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool cfg_ready(const app_cfg_t *c)
{
    return c != NULL
        && c->wifi_ssid[0] != '\0'
        && c->appid[0] != '\0'
        && c->token[0] != '\0'
        && c->voice[0] != '\0';
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
