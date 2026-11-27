/*
 * wifi_prov.c - Wi-Fi STA 连接 + SoftAP 常开控制台
 *
 * 运行形态：APSTA
 *   AP 侧常驻热点 kskbl-setup（192.168.4.1），手机随时可以连上来打开控制台，
 *   改配置、试听、看状态。这样即使路由器换了密码导致 STA 掉线，也不会失联。
 *   STA 侧负责联网调 DeepSeek 语音合成。
 *
 * 视觉风格对齐用户既有的《小智 AI 控制台》：
 *   配色 token（--bg/--blue/--teal…）、12px 圆角卡片、badge 徽章、note 提示条、chip 胶囊。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "ble_bridge.h"
#include "cfg_store.h"
#include "ds_chat.h"
#include "log_ring.h"
#include "speak.h"
#include "wifi_prov.h"

static const char *TAG = "wifi";

#define PROV_STA_RETRY      3
#define WIFI_BIT_CONNECTED  BIT0

static EventGroupHandle_t s_events;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static httpd_handle_t     s_httpd;
static int                s_retry;

/* ============================== 控制台页面 ============================== */
/* 页面正文在 page_html.h 中，由 tools/gen_page.py 从根目录 控制台.html 生成。
   改界面：改 HTML → 重跑该脚本 → 编译烧录，不必手改 C 字符串。 */
#include "page_html.h"

/* ============================== 工具函数 ============================== */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p != '\0'; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = { p[1], p[2], '\0' };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* JSON 字符串转义（只用于把用户填的值塞进响应里） */
static void json_esc(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0' && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", *p);
        } else {
            dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
}

typedef struct {
    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    char engine[CFG_ENGINE_MAX];
    char key[CFG_KEY_MAX];              /* DeepSeek API Key */
    char voice[CFG_VOICE_MAX];
    char db_appid[CFG_APPID_MAX];
    char db_token[CFG_TOKEN_MAX];
    char db_cluster[CFG_CLUSTER_MAX];
    char db_voice[CFG_DBVOICE_MAX];
    char text[512];                     /* 试听文本，独立字段 */
    bool has_wifi;
    bool has_engine;
    bool has_key;
    bool has_voice;
    bool has_db_appid;
    bool has_db_token;
    bool has_db_cluster;
    bool has_db_voice;
} form_t;

/* 敏感字段的规则统一为「留空 = 不修改」，避免只想切引擎时把密钥抹掉 */
static void take_str(char *dst, size_t cap, const char *v, bool *flag)
{
    if (v[0] != '\0') {
        strlcpy(dst, v, cap);
        *flag = true;
    }
}

static void form_apply(form_t *f, const char *k, const char *v)
{
    if (strcmp(k, "ssid") == 0) {
        strlcpy(f->ssid, v, sizeof(f->ssid));
        f->has_wifi = true;
    } else if (strcmp(k, "pass") == 0) {
        strlcpy(f->pass, v, sizeof(f->pass));
        f->has_wifi = true;
    } else if (strcmp(k, "engine") == 0) {
        if (cfg_engine_valid(v)) {
            strlcpy(f->engine, v, sizeof(f->engine));
            f->has_engine = true;
        }
    } else if (strcmp(k, "key") == 0) {
        take_str(f->key, sizeof(f->key), v, &f->has_key);
    } else if (strcmp(k, "voice") == 0) {
        if (cfg_voice_valid(v)) {
            strlcpy(f->voice, v, sizeof(f->voice));
            f->has_voice = true;
        }
    } else if (strcmp(k, "db_appid") == 0) {
        take_str(f->db_appid, sizeof(f->db_appid), v, &f->has_db_appid);
    } else if (strcmp(k, "db_token") == 0) {
        take_str(f->db_token, sizeof(f->db_token), v, &f->has_db_token);
    } else if (strcmp(k, "db_cluster") == 0) {
        take_str(f->db_cluster, sizeof(f->db_cluster), v, &f->has_db_cluster);
    } else if (strcmp(k, "db_voice") == 0) {
        take_str(f->db_voice, sizeof(f->db_voice), v, &f->has_db_voice);
    } else if (strcmp(k, "text") == 0) {
        strlcpy(f->text, v, sizeof(f->text));
    }
}

/* 读出请求体并解析成 form_t（只覆盖表单里出现的字段） */
static esp_err_t read_form(httpd_req_t *req, form_t *f)
{
    memset(f, 0, sizeof(*f));

    char *body = calloc(1, 2048);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int remaining = (int)req->content_len;
    if (remaining > 2047) {
        remaining = 2047;
    }
    int total = 0;
    while (total < remaining) {
        int r = httpd_req_recv(req, body + total, (size_t)(remaining - total));
        if (r <= 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char *p = body;
    while (p != NULL && *p != '\0') {
        char *amp = strchr(p, '&');
        if (amp != NULL) {
            *amp = '\0';
        }
        char *eq = strchr(p, '=');
        if (eq != NULL) {
            *eq = '\0';
            url_decode(p);
            url_decode(eq + 1);
            form_apply(f, p, eq + 1);
        }
        p = (amp != NULL) ? amp + 1 : NULL;
    }

    free(body);
    return ESP_OK;
}

static void reply_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
}

/* ============================== API ============================== */
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_state(httpd_req_t *req)
{
    app_cfg_t c;
    if (cfg_load(&c) != ESP_OK) {
        cfg_set_defaults(&c);
    }

    bool online = wifi_sta_connected();
    char ip[20] = "-";
    if (online && s_sta_netif != NULL) {
        esp_netif_ip_info_t ii;
        if (esp_netif_get_ip_info(s_sta_netif, &ii) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ii.ip));
        }
    }

    char e_ssid[CFG_SSID_MAX * 2];
    char e_voice[CFG_VOICE_MAX * 2];
    char e_engine[CFG_ENGINE_MAX * 2];
    char e_appid[CFG_APPID_MAX * 2];
    char e_cluster[CFG_CLUSTER_MAX * 2];
    char e_dbvoice[CFG_DBVOICE_MAX * 2];
    json_esc(c.wifi_ssid, e_ssid, sizeof(e_ssid));
    json_esc(c.voice, e_voice, sizeof(e_voice));
    json_esc(c.engine, e_engine, sizeof(e_engine));
    json_esc(c.db_appid, e_appid, sizeof(e_appid));
    json_esc(c.db_cluster, e_cluster, sizeof(e_cluster));
    json_esc(c.db_voice, e_dbvoice, sizeof(e_dbvoice));

    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"wifi\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"ble\":%d,\"ready\":%d,"
        "\"engine\":\"%s\",\"voice\":\"%s\","
        "\"has_key\":%d,\"has_token\":%d,"
        "\"db_appid\":\"%s\",\"db_cluster\":\"%s\",\"db_voice\":\"%s\","
        "\"busy\":%d}",
        online ? 1 : 0, ip, e_ssid,
        ble_bridge_connected() ? 1 : 0,
        cfg_ready(&c) ? 1 : 0,
        e_engine,
        e_voice,
        c.api_key[0] ? 1 : 0,       /* 只回「有没有」，Key 本身不回给前端 */
        c.db_token[0] ? 1 : 0,
        e_appid, e_cluster, e_dbvoice,
        speak_busy() ? 1 : 0);

    reply_json(req, buf);
    return ESP_OK;
}

static esp_err_t api_save(httpd_req_t *req)
{
    form_t f;
    if (read_form(req, &f) != ESP_OK) {
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    app_cfg_t c;
    if (cfg_load(&c) != ESP_OK) {
        cfg_set_defaults(&c);
    }

    if (f.has_wifi) {
        strlcpy(c.wifi_ssid, f.ssid, sizeof(c.wifi_ssid));
        strlcpy(c.wifi_pass, f.pass, sizeof(c.wifi_pass));
    }
    if (f.has_engine) {
        strlcpy(c.engine, f.engine, sizeof(c.engine));
    }
    if (f.has_key) {
        strlcpy(c.api_key, f.key, sizeof(c.api_key));
    }
    if (f.has_voice) {
        strlcpy(c.voice, f.voice, sizeof(c.voice));
    }
    if (f.has_db_appid) {
        strlcpy(c.db_appid, f.db_appid, sizeof(c.db_appid));
    }
    if (f.has_db_token) {
        strlcpy(c.db_token, f.db_token, sizeof(c.db_token));
    }
    if (f.has_db_cluster) {
        strlcpy(c.db_cluster, f.db_cluster, sizeof(c.db_cluster));
    }
    if (f.has_db_voice) {
        strlcpy(c.db_voice, f.db_voice, sizeof(c.db_voice));
    }

    esp_err_t err = cfg_save(&c);
    if (err != ESP_OK) {
        reply_json(req, "{\"ok\":0,\"err\":\"写入 NVS 失败\"}");
        return ESP_OK;
    }
    speak_reload_cfg();

    /* Wi-Fi 变了就重连；Key / 音色只需刷配置 */
    if (f.has_wifi && c.wifi_ssid[0] != '\0') {
        wifi_sta_connect(c.wifi_ssid, c.wifi_pass);
    }

    reply_json(req, "{\"ok\":1}");
    return ESP_OK;
}

static esp_err_t api_test(httpd_req_t *req)
{
    form_t f;
    if (read_form(req, &f) != ESP_OK) {
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    if (!wifi_sta_connected()) {
        reply_json(req, "{\"ok\":0,\"err\":\"Wi-Fi 未连接\"}");
        return ESP_OK;
    }
    app_cfg_t c;
    if (cfg_load(&c) != ESP_OK || !cfg_ready(&c)) {
        reply_json(req, "{\"ok\":0,\"err\":\"API Key 未配置\"}");
        return ESP_OK;
    }

    const char *text = (f.text[0] != '\0') ? f.text : "你好，我是语音合成模块";
    if (speak_say(text) != ESP_OK) {
        reply_json(req, "{\"ok\":0,\"err\":\"播放任务未就绪\"}");
        return ESP_OK;
    }
    reply_json(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 网页看日志：GET /api/logs?id=N；不带 id 表示“给我最近 60 行” */
static esp_err_t api_logs(httpd_req_t *req)
{
    char q[64] = {0};
    char v[24] = {0};
    bool has_id = false;
    uint32_t id = 0;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "id", v, sizeof(v)) == ESP_OK) {
        id = (uint32_t)strtoul(v, NULL, 10);
        has_id = true;
    }

    uint32_t cur   = log_ring_next();
    uint32_t since = id;
    if (!has_id) {
        since = (cur > 60u) ? (cur - 60u) : 0u;      /* 首次进来只倒最近几十行 */
    }

    char *raw = malloc(1500);
    if (raw == NULL) {
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    int n = log_ring_drain(since, raw, 1500);

    size_t cap = (size_t)n * 6 + 96;
    char *out = malloc(cap);
    if (out == NULL) {
        free(raw);
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    int plen = snprintf(out, cap, "{\"ok\":1,\"next\":%u,\"text\":\"", (unsigned)cur);
    int elen = (plen > 0)
             ? ds_chat_json_escape(raw, out + plen, (int)(cap - (size_t)plen - 3))
             : -1;
    free(raw);

    if (plen <= 0 || elen < 0) {
        free(out);
        reply_json(req, "{\"ok\":0,\"err\":\"日志组装失败\"}");
        return ESP_OK;
    }

    out[plen + elen]     = '"';
    out[plen + elen + 1] = '}';
    out[plen + elen + 2] = '\0';

    reply_json(req, out);
    free(out);
    return ESP_OK;
}

/* 一问一答：把问题送去 DeepSeek 的 chat/completions，把文字回答原样带回浏览器。
   注意：这里会阻塞到接口返回（最长 CHAT_TIMEOUT_MS），期间 httpd 不处理别的请求。 */
#define CHAT_REPLY_MAX  2048

static esp_err_t api_chat(httpd_req_t *req)
{
    form_t f;
    if (read_form(req, &f) != ESP_OK) {
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    if (f.text[0] == '\0') {
        reply_json(req, "{\"ok\":0,\"err\":\"说点什么再发\"}");
        return ESP_OK;
    }

    if (!wifi_sta_connected()) {
        reply_json(req, "{\"ok\":0,\"err\":\"板子还没连上 Wi-Fi，出不了网\"}");
        return ESP_OK;
    }

    app_cfg_t c;
    if (cfg_load(&c) != ESP_OK || c.api_key[0] == '\0') {
        reply_json(req, "{\"ok\":0,\"err\":\"还没填 DeepSeek API Key\"}");
        return ESP_OK;
    }

    char *reply = malloc(CHAT_REPLY_MAX);
    if (reply == NULL) {
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    int status = 0;
    esp_err_t err = ds_chat_ask(c.api_key, f.text, reply, CHAT_REPLY_MAX, &status);

    if (err != ESP_OK) {
        char msg[120];

        if (err == ESP_ERR_INVALID_SIZE) {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"问得太长了，分几句问\"}");
        } else if (status == 401) {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"API Key 无效（401）\"}");
        } else if (status == 402) {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"账户余额不足（402）\"}");
        } else if (status == 429) {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"请求太频繁（429）\"}");
        } else if (status > 0) {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"接口返回 HTTP %d\"}", status);
        } else {
            snprintf(msg, sizeof(msg), "{\"ok\":0,\"err\":\"连不上 api.deepseek.com\"}");
        }

        reply_json(req, msg);
        free(reply);
        return ESP_OK;
    }

    /* 回答可能带引号、换行，直接拼会破坏 JSON，必须转义。预留 6 倍余量。 */
    size_t len = strlen(reply);
    size_t cap = len * 6 + 64;
    char *out = malloc(cap);
    if (out == NULL) {
        free(reply);
        reply_json(req, "{\"ok\":0,\"err\":\"内存不足\"}");
        return ESP_OK;
    }

    const char *head = "{\"ok\":1,\"reply\":\"";
    size_t hlen = strlen(head);
    memcpy(out, head, hlen);

    int elen = ds_chat_json_escape(reply, out + hlen, (int)(cap - hlen - 3));
    if (elen < 0) {
        free(reply);
        free(out);
        reply_json(req, "{\"ok\":0,\"err\":\"回答太长，无法返回\"}");
        return ESP_OK;
    }

    out[hlen + (size_t)elen]     = '"';
    out[hlen + (size_t)elen + 1] = '}';
    out[hlen + (size_t)elen + 2] = '\0';

    reply_json(req, out);

    free(reply);
    free(out);
    return ESP_OK;
}

static esp_err_t api_reset(httpd_req_t *req)
{
    cfg_erase();
    reply_json(req, "{\"ok\":1}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

/* ============================== Wi-Fi 事件 ============================== */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_BIT_CONNECTED);
        if (s_retry < PROV_STA_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "STA 断开，第 %d 次重试", s_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "重试 %d 次仍失败，打开 http://%s/ 改配置",
                     PROV_STA_RETRY, PROV_AP_IP);
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "手机已连上配网热点");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "拿到 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, WIFI_BIT_CONNECTED);
    }
}

/* ============================== 对外接口 ============================== */
esp_err_t wifi_core_init(void)
{
    if (s_events != NULL) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif 初始化失败");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi 初始化失败");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "注册 WIFI_EVENT 失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL),
        TAG, "注册 IP_EVENT 失败");

    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass != NULL) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    }
    wc.sta.threshold.authmode = (wc.sta.password[0] != '\0')
                              ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry = 0;
    /* APSTA：切模式时不要关掉常开的配网热点 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "设置 APSTA 失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "写入 STA 配置失败");

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "wifi start 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "正在连接 %s ...", ssid);
    esp_wifi_connect();
    return ESP_OK;
}

bool wifi_sta_wait(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_BIT_CONNECTED,
                                          pdFALSE, pdTRUE,
                                          pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_BIT_CONNECTED) != 0;
}

bool wifi_sta_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_BIT_CONNECTED) != 0;
}

esp_err_t wifi_prov_start(void)
{
    wifi_config_t ac = { 0 };
    strlcpy((char *)ac.ap.ssid, PROV_AP_SSID, sizeof(ac.ap.ssid));
    ac.ap.ssid_len       = (uint8_t)strlen(PROV_AP_SSID);
    ac.ap.channel        = 6;
    ac.ap.max_connection = 4;
    if (strlen(PROV_AP_PASSWORD) > 0) {
        strlcpy((char *)ac.ap.password, PROV_AP_PASSWORD, sizeof(ac.ap.password));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ac.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "设置 APSTA 失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ac), TAG, "写入 AP 配置失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP 启动失败");

    if (s_httpd == NULL) {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.stack_size      = 12288;      /* 聊天要在 httpd 任务里跑 TLS 握手 */
        hc.lru_purge_enable = true;
        hc.max_uri_handlers = 8;
        ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "HTTP 服务启动失败");

        httpd_uri_t u_root  = { .uri = "/",           .method = HTTP_GET,  .handler = root_get  };
        httpd_uri_t u_state = { .uri = "/api/state",  .method = HTTP_GET,  .handler = api_state };
        httpd_uri_t u_save  = { .uri = "/api/save",   .method = HTTP_POST, .handler = api_save  };
        httpd_uri_t u_test  = { .uri = "/api/test",   .method = HTTP_POST, .handler = api_test  };
        httpd_uri_t u_chat  = { .uri = "/api/chat",   .method = HTTP_POST, .handler = api_chat  };
        httpd_uri_t u_logs  = { .uri = "/api/logs",   .method = HTTP_GET,  .handler = api_logs  };
        httpd_uri_t u_reset = { .uri = "/api/reset",  .method = HTTP_POST, .handler = api_reset };
        httpd_register_uri_handler(s_httpd, &u_root);
        httpd_register_uri_handler(s_httpd, &u_state);
        httpd_register_uri_handler(s_httpd, &u_save);
        httpd_register_uri_handler(s_httpd, &u_test);
        httpd_register_uri_handler(s_httpd, &u_chat);
        httpd_register_uri_handler(s_httpd, &u_logs);
        httpd_register_uri_handler(s_httpd, &u_reset);
    }

    ESP_LOGW(TAG, "控制台已开: 连热点 %s（密码%s），浏览器打开 http://%s/",
             PROV_AP_SSID,
             strlen(PROV_AP_PASSWORD) > 0 ? PROV_AP_PASSWORD : "无需",
             PROV_AP_IP);
    return ESP_OK;
}
