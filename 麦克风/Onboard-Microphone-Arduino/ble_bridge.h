/*
 * ble_bridge.h - BLE 双向通道（沿用旧模块的 GATT 参数）
 *
 * Service  6a400001-b5a3-f393-e0a9-e50e24dcca9e
 * Write    6a400002-b5a3-f393-e0a9-e50e24dcca9e   WRITE | WRITE NO RESPONSE
 * Notify   6a400003-b5a3-f393-e0a9-e50e24dcca9e   NOTIFY（CCCD 0x2902 自动生成）
 * 广播名   kskbl zdjd
 */
#pragma once

typedef void (*ble_rx_cb_t)(const char *text);

void ble_begin(ble_rx_cb_t rx_cb);

/* 给手机发一条通知，同时串口打印一份「【开发板】…」 */
void ble_send(const char *text);

bool ble_connected(void);
