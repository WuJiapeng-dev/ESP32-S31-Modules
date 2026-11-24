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
#define K_APIKEY    "apikey"
#define K_VOICE     "voice"

void cfg_set_defaults(app_cfg_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    strlcpy(c->voice, CFG_VOICE_DEFAULT, sizeof(c->voice));
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
    read_str(h, K_APIKEY, c->api_key, sizeof(c->api_key));
    read_str(h, K_VOICE, c->voice, sizeof(c->voice));

    nvs_close(h);

    /* 老固件（火山版）存过 voice=BV700_streaming 之类的值，换了引擎就不合法了，
     * 这里直接回落，否则会一直拿着无效音色去请求，每次都被 400 打回来 */
    if (!cfg_voice_valid(c->voice)) {
        ESP_LOGW(TAG, "音色 \"%s\" 不是 DeepSeek 的合法值，回落到 %s",
                 c->voice, CFG_VOICE_DEFAULT);
        strlcpy(c->voice, CFG_VOICE_DEFAULT, sizeof(c->voice));
    }

    ESP_LOGI(TAG, "配置: ssid=%s key=%s voice=%s",
             c->wifi_ssid[0] ? c->wifi_ssid : "(空)",
             c->api_key[0] ? "(已设置)" : "(空)",
             c->voice);
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
    if (err == ESP_OK) { err = nvs_set_str(h, K_APIKEY, c->api_key); }
    if (err == ESP_OK) { err = nvs_set_str(h, K_VOICE, c->voice); }
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
        && c->api_key[0] != '\0'
        && cfg_voice_valid(c->voice);
}

bool cfg_voice_valid(const char *voice)
{
    if (voice == NULL) {
        return false;
    }
    return strcmp(voice, CFG_VOICE_STELLA) == 0
        || strcmp(voice, CFG_VOICE_TIDE) == 0;
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
