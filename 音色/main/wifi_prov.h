/*
 * wifi_prov.h - Wi-Fi 连接 + 网页配网
 *
 * 设计：
 *   1) 开机读 NVS，配置齐全就直接连路由器（STA）；
 *   2) 配置缺失（或连不上）就起一个开放热点 kskbl-setup，手机连上后
 *      浏览器打开 http://192.168.4.1/ 填表，提交后写入 NVS 并重启。
 *
 * 密钥只走这条路径进芯片，源码和仓库里不出现任何 AppID / Token。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "cfg_store.h"

#define PROV_AP_SSID        "kskbl-setup"
#define PROV_AP_PASSWORD    ""              /* 空 = 开放热点，方便手机直接连 */
#define PROV_AP_IP          "192.168.4.1"

/* netif / event loop / wifi 栈初始化，进程内只调一次 */
esp_err_t wifi_core_init(void);

/* 以 STA 身份连指定 AP，内部会自动重连（最多 PROV_STA_RETRY 次） */
esp_err_t wifi_sta_connect(const char *ssid, const char *pass);

/* 等拿到 IP，超时返回 false */
bool wifi_sta_wait(uint32_t timeout_ms);

bool wifi_sta_connected(void);

/* 起配网热点 + HTTP 服务（不阻塞，返回后由 httpd 任务处理请求） */
esp_err_t wifi_prov_start(void);
