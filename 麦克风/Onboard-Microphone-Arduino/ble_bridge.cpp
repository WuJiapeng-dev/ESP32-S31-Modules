/*
 * ble_bridge.cpp - GATT 服务（arduino-esp32 4.0.0 的 BLE.h 新 API）
 *
 * 4.0.0 把旧的 BLEDevice/抽象回调类整套换掉了：现在是一个全局单例 BLE 对象 +
 * 共享句柄 + lambda 回调，CCCD(0x2902) 对 Notify 特征自动创建。
 * 迁移依据：核心包自带的 libraries/BLE/MIGRATION.md（v3.x → v4.0）、
 * 以及 libraries/BLE/examples/Notify/Notify.ino。
 */
#include "ble_bridge.h"

#include <Arduino.h>
#include <BLE.h>
#include <string.h>

#define DEVICE_NAME "kskbl zdjd"

static const BLEUUID SVC_UUID("6a400001-b5a3-f393-e0a9-e50e24dcca9e");
static const BLEUUID CHR_WRITE_UUID("6a400002-b5a3-f393-e0a9-e50e24dcca9e");
static const BLEUUID CHR_NOTIFY_UUID("6a400003-b5a3-f393-e0a9-e50e24dcca9e");

static BLECharacteristic s_notify;
static bool              s_connected = false;
static ble_rx_cb_t       s_rx_cb = NULL;

void ble_begin(ble_rx_cb_t rx_cb)
{
    s_rx_cb = rx_cb;

    BTStatus st = BLE.begin(DEVICE_NAME);
    if (!st) {
        Serial.printf("【蓝牙】初始化失败：%s\r\n", st.toString());
        return;
    }

    BLEServer server = BLE.createServer();
    if (!server) {
        Serial.println("【蓝牙】创建 GATT 服务端失败");
        return;
    }

    server.onConnect([](BLEServer s, const BLEConnInfo &conn) {
        (void)s;
        s_connected = true;
        Serial.printf("【蓝牙】手机已连接（MTU %d）\r\n", conn.getMTU());
    });

    server.onDisconnect([](BLEServer s, const BLEConnInfo &conn, uint8_t reason) {
        (void)s;
        (void)conn;
        s_connected = false;
        Serial.printf("【蓝牙】手机已断开（原因 0x%02X），重新开始广播\r\n", reason);
        BLE.startAdvertising();
    });

    BLEService svc = server.createService(SVC_UUID);
    if (!svc) {
        Serial.println("【蓝牙】创建服务失败");
        return;
    }

    BLECharacteristic chr_write = svc.createCharacteristic(
        CHR_WRITE_UUID,
        BLEProperty::Write | BLEProperty::WriteNR,
        BLEPermission::WriteOpen);
    if (!chr_write) {
        Serial.println("【蓝牙】创建写特征失败");
        return;
    }

    chr_write.onWrite([](BLECharacteristic &c, const BLEConnInfo &conn) {
        (void)conn;
        size_t n = 0;
        const uint8_t *p = c.getValue(&n);
        if (p == NULL || n == 0) {
            return;
        }

        char buf[200];
        size_t cp = (n < sizeof(buf) - 1) ? n : (sizeof(buf) - 1);
        memcpy(buf, p, cp);
        buf[cp] = '\0';

        Serial.printf("【手机】%s\r\n", buf);
        if (s_rx_cb != NULL) {
            s_rx_cb(buf);
        }
    });

    /* Notify 特征的 CCCD(0x2902) 由协议栈自动创建，不用手动加描述符 */
    s_notify = svc.createCharacteristic(CHR_NOTIFY_UUID,
                                        BLEProperty::Notify,
                                        BLEPermission::ReadOpen);
    if (!s_notify) {
        Serial.println("【蓝牙】创建通知特征失败");
        return;
    }

    st = server.start();
    if (!st) {
        Serial.printf("【蓝牙】启动服务端失败：%s\r\n", st.toString());
        return;
    }

    BLEAdvertising adv = BLE.getAdvertising();
    adv.addServiceUUID(SVC_UUID);
    st = adv.start();
    if (!st) {
        Serial.printf("【蓝牙】启动广播失败：%s\r\n", st.toString());
        return;
    }

    Serial.printf("【蓝牙】GATT 已就绪：6a400001 / 0002(写) / 0003(通知)\r\n");
    Serial.printf("【蓝牙】开始广播，设备名 \"%s\"\r\n", DEVICE_NAME);
}

void ble_send(const char *text)
{
    if (text == NULL) {
        return;
    }

    /* 双向可见：板子发的每一条内容，串口也留一份 */
    Serial.printf("【开发板】%s\r\n", text);

    if (!s_connected) {
        Serial.println("【蓝牙】当前无手机连接，本条仅串口可见");
        return;
    }
    if (!s_notify) {
        return;
    }
    if (s_notify.getSubscribedCount() == 0) {
        Serial.println("【蓝牙】手机还没订阅 6a400003 的 Notify，本条仅串口可见");
        return;
    }

    s_notify.notify((const uint8_t *)text, strlen(text));
}

bool ble_connected(void)
{
    return s_connected;
}
