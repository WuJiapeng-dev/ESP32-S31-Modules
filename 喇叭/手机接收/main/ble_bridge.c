/*
 * ble_bridge.c - BLE（NimBLE）GATT 服务 + 广播
 *
 * 服务/特征 UUID 沿用本对话上一轮已确定的同一套（手机调试助手可直接复用同一配置）：
 *   Service        6a400001-b5a3-f393-e0a9-e50e24dcca9e  (Primary)
 *   Char  Write    6a400002-b5a3-f393-e0a9-e50e24dcca9e  WRITE | WRITE NO RESPONSE（手机 -> ESP32）
 *   Char  Notify   6a400003-b5a3-f393-e0a9-e50e24dcca9e  NOTIFY（CCCD 0x2902 由协议栈自动生成）
 *
 * 广播名：kskbl zdjd（含 1 个空格，逐字符一致）
 *
 * 与上一轮的区别：写特征收到的是文本命令（ASR / EN:标签 / LIST / SAY:文本 …），
 * 交给 asr.c 的 app_cmd_feed() 解析。本工程没有喇叭，非命令文本回 ERR:UNKNOWN。
 */
#include "ble_bridge.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "asr.h"

#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"


static const char *TAG = "ble_bridge";

#define DEVICE_NAME         "kskbl zdjd"   /* 用户指定：含空格，逐字符一致 */
#define ADV_INSTANCE        0
#define WRITE_CHUNK_MAX     256            /* 单次 ATT 写最大负载（MTU 247 时是 244） */
#define NOTIFY_MTU_MAX      20             /* 保守按默认 MTU 23 计算，单包有效负载 20 字节 */
#define PREFERRED_MTU       247            /* 主动协商大 MTU，让手机一次能发长句 */

/*
 * 128-bit UUID 在 NimBLE / BLE 空口上都是小端字节序：
 * value[0] 对应 UUID 字符串的最后一字节。
 * "6a400001-b5a3-f393-e0a9-e50e24dcca9e" -> 9e ca dc 24 0e e5 a9 e0 93 f3 a3 b5 01 00 40 6a
 */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6a);
static const ble_uuid128_t s_chr_write_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6a);
static const ble_uuid128_t s_chr_notify_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6a);

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_val_handle;
static uint16_t s_notify_val_handle;
static bool     s_notify_enabled;
static uint8_t  s_own_addr_type;

static int  ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void ble_advertise(void);
static int  ble_set_adv_payload(uint8_t instance, const uint8_t *buf,
                                size_t len, bool is_scan_rsp);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* 手机 -> ESP32：可写、可无响应写 */
                .uuid       = &s_chr_write_uuid.u,
                .access_cb  = ble_gatt_access,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_write_val_handle,
            }, {
                /* ESP32 -> 手机：通知（CCCD 由 NimBLE 自动添加） */
                .uuid       = &s_chr_notify_uuid.u,
                .access_cb  = ble_gatt_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_notify_val_handle,
            }, {
                0, /* 特征结束 */
            }
        },
    },
    {
        0, /* 服务结束 */
    }
};

/* ------------------------------------------------------------------ */
/* GATT 访问回调                                                       */
/* ------------------------------------------------------------------ */
static int ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        if (attr_handle != s_write_val_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint16_t pkt_len = OS_MBUF_PKTLEN(ctxt->om);
        if (pkt_len > WRITE_CHUNK_MAX) {
            ESP_LOGW(TAG, "单次写入过长 (%u 字节)，已拒绝", pkt_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t buf[WRITE_CHUNK_MAX];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, WRITE_CHUNK_MAX, &len);
        if (rc != 0) {
            ESP_LOGW(TAG, "读取写入内容失败 rc=%d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG, "收到 BLE 写入 %u 字节", len);
        app_cmd_feed(buf, len);      /* 只拷贝，解析放到 asr 任务里做 */
        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* 本工程两个特征都不开放读 */
        return BLE_ATT_ERR_READ_NOT_PERMITTED;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ------------------------------------------------------------------ */
/* 广播                                                                */
/* ------------------------------------------------------------------ */
static int ble_set_adv_payload(uint8_t instance, const uint8_t *buf,
                               size_t len, bool is_scan_rsp)
{
    struct os_mbuf *data = os_msys_get_pkthdr((uint16_t)len, 0);
    if (data == NULL) {
        return BLE_HS_ENOMEM;
    }

    int rc = os_mbuf_append(data, buf, (uint16_t)len);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        return rc;
    }

    rc = is_scan_rsp ? ble_gap_ext_adv_rsp_set_data(instance, data)
                     : ble_gap_ext_adv_set_data(instance, data);
    if (rc != 0) {
        /* 失败时 mbuf 归调用者释放 */
        os_mbuf_free_chain(data);
    }
    return rc;
}

static void ble_advertise(void)
{
    if (ble_gap_ext_adv_active(ADV_INSTANCE)) {
        return;
    }

    struct ble_gap_ext_adv_params params;
    memset(&params, 0, sizeof(params));
    params.connectable    = 1;
    params.scannable      = 1;
    params.legacy_pdu     = 1;      /* 用传统 ADV_IND 空口格式，兼容性最好 */
    params.own_addr_type  = s_own_addr_type;
    params.primary_phy    = BLE_HCI_LE_PHY_1M;
    params.secondary_phy  = BLE_HCI_LE_PHY_1M;
    params.tx_power       = 127;    /* 0x7F：不指定，用控制器默认 */
    params.itvl_min       = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max       = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    params.sid            = 0;

    int rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &params, NULL, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure 失败 rc=%d", rc);
        return;
    }

    /* 广播包：Flags + 完整设备名 */
    uint8_t adv[31];
    size_t n = 0;
    const char *name = DEVICE_NAME;
    size_t name_len = strlen(name);

    adv[n++] = 0x02;
    adv[n++] = BLE_HS_ADV_TYPE_FLAGS;
    adv[n++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv[n++] = (uint8_t)(name_len + 1);
    adv[n++] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(&adv[n], name, name_len);
    n += name_len;

    rc = ble_set_adv_payload(ADV_INSTANCE, adv, n, false);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d", rc);
        return;
    }

    /* 扫描响应：128-bit 服务 UUID，手机扫描时就能看到本服务 */
    uint8_t rsp[31];
    size_t m = 0;
    rsp[m++] = 0x11;    /* 长度 = 1 (type) + 16 (uuid) */
    rsp[m++] = BLE_HS_ADV_TYPE_COMP_UUIDS128;
    memcpy(&rsp[m], s_svc_uuid.value, 16);
    m += 16;

    rc = ble_set_adv_payload(ADV_INSTANCE, rsp, m, true);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败 rc=%d", rc);
        return;
    }

    rc = ble_gap_ext_adv_start(ADV_INSTANCE, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "开始广播，设备名 \"%s\"", DEVICE_NAME);
}

/* ------------------------------------------------------------------ */
/* GAP 事件                                                            */
/* ------------------------------------------------------------------ */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "手机已连接 conn_handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "连接失败 status=%d，重新广播", event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "连接断开 reason=%d，重新广播", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_notify_val_handle) {
            s_notify_enabled = (event->subscribe.cur_notify != 0);
            ESP_LOGI(TAG, "Notify 订阅：%s", s_notify_enabled ? "已开启" : "已关闭");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 已协商为 %d（单包可发 %d 字节）",
                 event->mtu.value, event->mtu.value - 3);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "广播结束 reason=%d，重新广播", event->adv_complete.reason);
        ble_advertise();
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
esp_err_t ble_bridge_init(void)
{
    /* 主动请求较大 MTU：手机发长中文时不必拆成很多 20 字节的小包 */
    int rc = ble_att_set_preferred_mtu(PREFERRED_MTU);
    if (rc != 0) {
        ESP_LOGW(TAG, "设置首选 MTU 失败 rc=%d，沿用默认", rc);
    }

    /* 注意：这两个函数在 IDF v6.1 的 NimBLE 里返回 void */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置设备名失败 rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败 rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败 rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GATT 服务已注册：6a400001/0002/0003-b5a3-f393-e0a9-e50e24dcca9e");
    return ESP_OK;
}

void ble_bridge_start(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr 失败 rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto 失败 rc=%d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
        ESP_LOGI(TAG, "本机 BLE 地址 %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], s_own_addr_type);
    }

    ble_advertise();
}

void ble_bridge_notify(const char *text)
{
    if (text == NULL) {
        return;
    }

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGD(TAG, "当前无连接，跳过 Notify");
        return;
    }
    if (!s_notify_enabled) {
        ESP_LOGW(TAG, "客户端未订阅 Notify，跳过 \"%s\"", text);
        return;
    }

    size_t len = strlen(text);
    if (len == 0 || len > NOTIFY_MTU_MAX) {
        ESP_LOGW(TAG, "Notify 长度不合法 (%u 字节)", (unsigned)len);
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, (uint16_t)len);
    if (om == NULL) {
        ESP_LOGE(TAG, "mbuf 分配失败，Notify 放弃");
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_notify_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify 发送失败 rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "已 Notify: \"%s\"", text);
}

bool ble_bridge_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
