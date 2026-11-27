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

#include "audio_out.h"     /* 网页「试听」按钮直接推喇叭 */
#include "cfg_store.h"
#include "dialog.h"        /* dialog_is_speaking()：网页状态用 */
#include "ds_tts.h"        /* 网页「试听」按钮走 DeepSeek TTS */
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
static const char PAGE_HTML[] =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
"<title>云端语音合成控制台</title><style>"
":root{--bg:#f4f7fb;--card:#fff;--ink:#182334;--muted:#637189;--line:#e2e9f2;"
"--blue:#2563eb;--blue-s:#edf4ff;--teal:#0d9488;--teal-s:#e8f8f5;"
"--purple:#7c3aed;--purple-s:#f3edff;--amber:#b45309;--amber-s:#fff5e6;"
"--red:#c0392b;--red-s:#fdf0ee;--code:#f7f9fc}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--bg);color:var(--ink);"
"font:14px/1.65 -apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif}"
".wrap{max-width:760px;margin:0 auto;padding-bottom:40px}"
".top{position:sticky;top:0;z-index:9;background:rgba(244,247,251,.96);"
"backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:14px 16px 12px}"
".logo{font-size:17px;font-weight:700;letter-spacing:.3px}"
".logo span{color:var(--blue)}"
".badges{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}"
".badge{font-size:11px;padding:3px 9px;border-radius:20px;font-weight:600;white-space:nowrap}"
".b-blue{background:var(--blue-s);color:var(--blue)}"
".b-teal{background:var(--teal-s);color:var(--teal)}"
".b-purple{background:var(--purple-s);color:var(--purple)}"
".b-amber{background:var(--amber-s);color:var(--amber)}"
".b-red{background:var(--red-s);color:var(--red)}"
".b-gray{background:#eef1f6;color:#5c6a7e}"
".tabs{display:flex;gap:6px;padding:12px 16px 0;overflow-x:auto}"
".nav{border:1px solid var(--line);background:#fff;color:var(--muted);"
"padding:7px 14px;border-radius:20px;font-size:13px;white-space:nowrap;cursor:pointer;font-family:inherit}"
".nav.on{background:var(--blue-s);border-color:#bcd3ff;color:var(--blue);font-weight:600}"
"main{padding:14px 16px 0}"
".view{display:none}.view.on{display:block}"
".card{background:var(--card);border:1px solid var(--line);border-radius:12px;"
"padding:18px 18px 20px;margin-bottom:14px}"
".card h2{font-size:16px;margin:0 0 4px}"
".desc{color:var(--muted);font-size:12.5px;margin:0 0 14px}"
"label.f{display:block;font-size:12.5px;color:var(--muted);margin:0 0 5px;font-weight:600}"
"input,textarea{width:100%;border:1px solid var(--line);border-radius:8px;padding:9px 11px;"
"font:inherit;font-size:13px;background:#fff;color:var(--ink)}"
"input:focus,textarea:focus{outline:2px solid var(--blue-s);border-color:var(--blue)}"
"textarea{min-height:88px;resize:vertical}"
".field{margin-bottom:12px}"
"input[type=range]{padding:0;border:0;height:26px;background:transparent}"
".btn{border:1px solid var(--line);background:#fff;color:var(--ink);border-radius:8px;"
"padding:8px 14px;font-size:13px;cursor:pointer;font-family:inherit}"
".btn.primary{background:var(--blue);border-color:var(--blue);color:#fff;font-weight:600;width:100%;padding:11px}"
".btn.primary:active{background:#1259c3}"
".btn.danger{color:var(--red);width:100%;padding:10px}"
".note{border-left:4px solid var(--blue);background:var(--blue-s);padding:10px 13px;"
"border-radius:0 8px 8px 0;font-size:12.4px;margin:12px 0}"
".note.warn{border-color:var(--amber);background:var(--amber-s)}"
".note.ok{border-color:var(--teal);background:var(--teal-s)}"
".pill-line{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0 14px}"
".chip{border:1px solid var(--line);background:#fff;border-radius:20px;"
"padding:5px 12px;font-size:12px;cursor:pointer}"
".chip.on{background:var(--blue-s);border-color:#bcd3ff;color:var(--blue);font-weight:600}"
"code{font-family:Consolas,Menlo,monospace;font-size:11.8px;background:var(--code);"
"border:1px solid var(--line);border-radius:4px;padding:1px 5px;color:#164a86}"
"table{width:100%;border-collapse:collapse;font-size:12.6px}"
"th,td{border:1px solid var(--line);padding:7px 10px;text-align:left}"
"td:first-child{color:var(--muted);width:42%}"
"#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:24px;z-index:99;"
"display:flex;flex-direction:column;gap:8px;align-items:center}"
".toast{background:#182334;color:#fff;padding:10px 16px;border-radius:9px;"
"font-size:12.8px;box-shadow:0 6px 22px rgba(0,0,0,.16)}"
"</style></head><body><div class='wrap'>"

"<div class='top'><div class='logo'>云端 <span>语音合成控制台</span></div>"
"<div class='badges'>"
"<span class='badge b-gray' id='bWifi'>Wi-Fi 未知</span>"
"<span class='badge b-gray' id='bBle'>BLE 未知</span>"
"<span class='badge b-gray' id='bEng'>引擎 未知</span>"
"</div></div>"

"<div class='tabs'>"
"<button class='nav on' data-t='conn'>连接</button>"
"<button class='nav' data-t='eng'>引擎</button>"
"<button class='nav' data-t='voice'>音色</button>"
"<button class='nav' data-t='test'>试听</button>"
"</div><main>"

"<section class='view on' id='vConn'><div class='card'>"
"<h2>Wi-Fi 连接</h2>"
"<p class='desc'>只支持 2.4GHz 网络，5GHz 连不上。</p>"
"<div class='field'><label class='f'>网络名称 SSID</label>"
"<input id='ssid' maxlength='32' placeholder='例如 MyHome'></div>"
"<div class='field'><label class='f'>密码</label>"
"<input id='pass' type='password' maxlength='64' placeholder='开放网络可留空'></div>"
"<div class='note' id='ipNote'>当前未连接</div>"
"<button class='btn primary' id='btnConn'>保存并连接</button>"
"</div></section>"

"<section class='view' id='vEng'><div class='card'>"
"<h2>合成引擎</h2>"
"<p class='desc'>选一个引擎，填好它的凭据。两套凭据会同时保存，"
"以后想换回来不用重填。</p>"
"<div class='pill-line'>"
"<span class='chip eng on' data-e='deepseek'>DeepSeek</span>"
"<span class='chip eng' data-e='doubao'>豆包 · 火山引擎</span>"
"</div>"
"<div id='engDs'>"
"<p class='desc'>platform.deepseek.com → 左侧 API keys → 创建，得到一串 sk- 开头的密钥。"
"语音合成与对话共用同一个 Key。</p>"
"<div class='field'><label class='f'>API Key</label>"
"<input id='key' maxlength='127' placeholder='sk-xxxxxxxxxxxxxxxx' autocomplete='off'></div>"
"<div class='note'>只需要一个 Key，没有 AppID / Token / cluster 那一套。"
"鉴权头是 <code>Bearer &lt;key&gt;</code>，空格分隔。</div>"
"</div>"
"<div id='engDb' style='display:none'>"
"<p class='desc'>console.volcengine.com → 语音技术 → 语音合成，开通后在"
"「服务接口认证信息」里拿 AppID / Access Token / Cluster。</p>"
"<div class='field'><label class='f'>AppID</label>"
"<input id='db_appid' maxlength='31' placeholder='例如 3145xxxxxx' autocomplete='off'></div>"
"<div class='field'><label class='f'>Access Token</label>"
"<input id='db_token' maxlength='127' placeholder='控制台里的 Access Token' autocomplete='off'></div>"
"<div class='field'><label class='f'>Cluster</label>"
"<input id='db_cluster' maxlength='31' placeholder='volcano_tts'></div>"
"<div class='field'><label class='f'>voice_type</label>"
"<input id='db_voice' maxlength='63' placeholder='BV700_streaming'></div>"
"<div class='note warn'>鉴权头是 <code>Bearer;&lt;token&gt;</code>，<b>分号</b>分隔，"
"不是空格 —— 这里最容易写错。</div>"
"<div class='note warn'>豆包后端<b>尚未接入</b>：V1 是非流式接口，整段音频以 base64"
"塞在 JSON 里一次返回，长句会撑爆内存。现在切过去只会回 <code>ERR:ENGINE</code>。</div>"
"</div>"
"<button class='btn primary' id='btnEng'>保存引擎配置</button>"
"</div></section>"

"<section class='view' id='vVoice'><div class='card'>"
"<h2>音色</h2>"
"<p class='desc'>这一页只对 DeepSeek 生效，它只有两个发音人。"
"切到豆包时音色在「引擎」页填 voice_type。</p>"
"<div class='pill-line'>"
"<span class='chip on' data-v='stella'>海星 · 女声</span>"
"<span class='chip' data-v='tide'>暗潮 · 男声</span>"
"</div>"
"<div class='field'><label class='f'>voice_id</label>"
"<input id='voice' maxlength='15' value='stella' readonly></div>"
"<div class='note warn'>这个接口<b>不支持调语速和音量</b>，官网上写明了。"
"要变速只能改文本本身（比如加「慢一点说」这类提示对 TTS 无效，实际得靠标点控制停顿）。</div>"
"<button class='btn primary' id='btnVoice'>保存</button>"
"</div></section>"

"<section class='view' id='vTest'><div class='card'>"
"<h2>试听</h2>"
"<p class='desc'>先把「连接」和「引擎」填好。设备正在播放时再点会排队。</p>"
"<div class='field'><textarea id='text'>你好，我是 ESP32-S31 语音模块，现在的声音来自云端合成。</textarea></div>"
"<button class='btn primary' id='btnTest'>播放试听</button>"
"</div>"
"<div class='card'><h2>设备状态</h2><table><tbody id='stateTable'>"
"<tr><td>加载中</td><td>…</td></tr></tbody></table></div>"
"<div class='card'><h2>恢复出厂</h2>"
"<p class='desc'>清空 Wi-Fi 与密钥，清完自动重启。</p>"
"<button class='btn danger' id='btnReset'>清空配置</button></div>"
"</section>"

"</main></div><div id='toast'></div><script>"
"var $=function(i){return document.getElementById(i)};"
"var state={};"
"function toast(m){var d=document.createElement('div');d.className='toast';d.textContent=m;"
"$('toast').appendChild(d);setTimeout(function(){d.remove()},2600)}"
"function setBadge(id,txt,cls){var e=$(id);e.textContent=txt;e.className='badge '+cls}"
"document.querySelectorAll('.nav').forEach(function(b){b.addEventListener('click',function(){"
"document.querySelectorAll('.nav').forEach(function(x){x.classList.remove('on')});"
"b.classList.add('on');"
"document.querySelectorAll('.view').forEach(function(v){v.classList.remove('on')});"
"$('v'+b.dataset.t.charAt(0).toUpperCase()+b.dataset.t.slice(1)).classList.add('on');"
"})});"
"var tVoice=0,tEng=0;"
"document.querySelectorAll('.chip[data-v]').forEach(function(c){c.addEventListener('click',function(){"
"$('voice').value=c.dataset.v;tVoice=1;"
"document.querySelectorAll('.chip[data-v]').forEach(function(x){x.classList.remove('on')});"
"c.classList.add('on')})});"
"function showEngine(e){"
"document.querySelectorAll('.eng').forEach(function(x){x.classList.toggle('on',x.dataset.e===e)});"
"$('engDs').style.display=(e==='deepseek')?'':'none';"
"$('engDb').style.display=(e==='doubao')?'':'none'}"
"document.querySelectorAll('.eng').forEach(function(c){c.addEventListener('click',function(){"
"tEng=1;showEngine(c.dataset.e)})});"
"function post(url,obj){var b=new URLSearchParams();"
"for(var k in obj){if(obj[k]!==undefined)b.set(k,obj[k])}"
"return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
".then(function(r){return r.json()}).catch(function(){return {ok:0,err:'网络错误'}})}"
"function fill(s){state=s;"
"if(s.wifi){setBadge('bWifi','Wi-Fi 已连接','b-teal')}else{setBadge('bWifi','Wi-Fi 未连接','b-red')}"
"setBadge('bBle',s.ble?'BLE 已连接':'BLE 广播中',s.ble?'b-purple':'b-gray');"
"var en=s.engine||'deepseek';"
"var enName=(en==='doubao')?'豆包':'DeepSeek';"
"setBadge('bEng',enName+(s.ready?' 就绪':' 未配置'),s.ready?'b-blue':'b-amber');"
"if(!tEng){showEngine(en)}"
"$('ipNote').textContent=s.wifi?('已连接 '+s.ssid+'，IP '+s.ip):'当前未连接，请填写下面的网络信息';"
"$('ipNote').className='note'+(s.wifi?' ok':'');"
"if(s.voice&&!tVoice){$('voice').value=s.voice;"
"document.querySelectorAll('.chip[data-v]').forEach(function(x){"
"x.classList.toggle('on',x.dataset.v===s.voice)})}"
"if(s.has_key){$('key').placeholder='已保存，留空表示不修改'}"
"if(s.has_token){$('db_token').placeholder='已保存，留空表示不修改'}"
"if(s.db_appid){$('db_appid').value=s.db_appid}"
"if(s.db_cluster){$('db_cluster').value=s.db_cluster}"
"if(s.db_voice){$('db_voice').value=s.db_voice}"
"if(s.ssid){$('ssid').value=s.ssid}"
"var rows=[['Wi-Fi',s.wifi?('已连接 '+s.ssid):'未连接'],['IP',s.wifi?s.ip:'-'],"
"['BLE',s.ble?'手机已连接':'广播中'],['引擎',enName+(s.ready?' · 就绪':' · 缺凭据')],"
"['音色',(en==='doubao')?(s.db_voice||'-'):(s.voice||'-')],"
"['播放',s.busy?'正在播放':'空闲'],['热点','kskbl-setup (192.168.4.1)']];"
"$('stateTable').innerHTML=rows.map(function(r){return '<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>'}).join('')"
"}"
"function poll(){fetch('/api/state').then(function(r){return r.json()}).then(function(s){"
"if(!state.init){fill(s);state.init=1}else{var ini=state.init;fill(s);state.init=ini}"
"}).catch(function(){setBadge('bWifi','控制台断开','b-red')})}"
"$('btnConn').addEventListener('click',function(){post('/api/save',{ssid:$('ssid').value,"
"pass:$('pass').value}).then(function(j){toast(j.ok?'已保存，正在连接…':(j.err||'失败'));setTimeout(poll,1200)})});"
"$('btnEng').addEventListener('click',function(){"
"var e=document.querySelector('.eng.on').dataset.e;"
"post('/api/save',{engine:e,key:$('key').value,"
"db_appid:$('db_appid').value,db_token:$('db_token').value,"
"db_cluster:$('db_cluster').value,db_voice:$('db_voice').value})"
".then(function(j){toast(j.ok?'引擎配置已保存':(j.err||'失败'));"
"if(j.ok){tEng=0;$('key').value='';$('db_token').value=''}poll()})});"
"$('btnVoice').addEventListener('click',function(){post('/api/save',{voice:$('voice').value})"
".then(function(j){toast(j.ok?'音色已保存':(j.err||'失败'));if(j.ok)tVoice=0;poll()})});"
"$('btnTest').addEventListener('click',function(){post('/api/test',{text:$('text').value})"
".then(function(j){toast(j.ok?'已发送，留意喇叭':(j.err||'失败'))})});"
"$('btnReset').addEventListener('click',function(){if(!confirm('确定清空全部配置？'))return;"
"post('/api/reset',{}).then(function(){toast('已清空，设备重启中…')})});"
"poll();setInterval(poll,3000);"
"</script></body></html>";

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
        0,                          /* 本工程不做 BLE，前端 ble 字段恒为 0 */
        cfg_ready(&c) ? 1 : 0,
        e_engine,
        e_voice,
        c.api_key[0] ? 1 : 0,       /* 只回「有没有」，Key 本身不回给前端 */
        c.db_token[0] ? 1 : 0,
        e_appid, e_cluster, e_dbvoice,
        dialog_is_speaking() ? 1 : 0);

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
    /* 对话任务每一轮都会重新 cfg_load()，这里不需要额外通知 */

    /* Wi-Fi 变了就重连；Key / 音色只需刷配置 */
    if (f.has_wifi && c.wifi_ssid[0] != '\0') {
        wifi_sta_connect(c.wifi_ssid, c.wifi_pass);
    }

    reply_json(req, "{\"ok\":1}");
    return ESP_OK;
}

static esp_err_t prov_tts_pcm(const int16_t *pcm, size_t samples, void *user)
{
    (void)user;
    return audio_out_write_mono16(pcm, samples);
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

    /* 网页「试听」：直接调 DeepSeek TTS，边收边推 MAX98357。
     * 注意这是在 httpd 任务里同步做一次 TLS 请求，stack_size 已调到 12KB，
     * 整个过程约 1~3 秒，期间网页会转圈，属预期。 */
    audio_out_set_enable(true);
    ds_result_t res = {0};
    esp_err_t err = ds_tts_speak(&c, text, prov_tts_pcm, NULL, &res);
    audio_out_drain();
    audio_out_set_enable(false);

    if (err != ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":0,\"err\":\"合成失败 HTTP %d\"}", res.http_status);
        reply_json(req, msg);
        return ESP_OK;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"ok\":1,\"bytes\":%u,\"ms\":%u}",
             (unsigned)res.pcm_bytes, (unsigned)res.elapsed_ms);
    reply_json(req, msg);
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
        hc.stack_size      = 12288;   /* 网页「试听」会在 httpd 任务里做一次 TLS 请求 */
        hc.lru_purge_enable = true;
        hc.max_uri_handlers = 8;
        ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "HTTP 服务启动失败");

        httpd_uri_t u_root  = { .uri = "/",           .method = HTTP_GET,  .handler = root_get  };
        httpd_uri_t u_state = { .uri = "/api/state",  .method = HTTP_GET,  .handler = api_state };
        httpd_uri_t u_save  = { .uri = "/api/save",   .method = HTTP_POST, .handler = api_save  };
        httpd_uri_t u_test  = { .uri = "/api/test",   .method = HTTP_POST, .handler = api_test  };
        httpd_uri_t u_reset = { .uri = "/api/reset",  .method = HTTP_POST, .handler = api_reset };
        httpd_register_uri_handler(s_httpd, &u_root);
        httpd_register_uri_handler(s_httpd, &u_state);
        httpd_register_uri_handler(s_httpd, &u_save);
        httpd_register_uri_handler(s_httpd, &u_test);
        httpd_register_uri_handler(s_httpd, &u_reset);
    }

    ESP_LOGW(TAG, "控制台已开: 连热点 %s（密码%s），浏览器打开 http://%s/",
             PROV_AP_SSID,
             strlen(PROV_AP_PASSWORD) > 0 ? PROV_AP_PASSWORD : "无需",
             PROV_AP_IP);
    return ESP_OK;
}
