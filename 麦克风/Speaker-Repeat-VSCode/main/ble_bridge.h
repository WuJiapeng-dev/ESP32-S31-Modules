/*
 * ble_bridge.h - BLE（NimBLE）GATT 服务 + 广播
 *
 * UUID 沿用上一个模块（chuankou）已定的同一套：
 *   Service     6a400001-b5a3-f393-e0a9-e50e24dcca9e  (Primary)
 *   Char Write  6a400002-b5a3-f393-e0a9-e50e24dcca9e  WRITE | WRITE NO RESPONSE
 *   Char Notify 6a400003-b5a3-f393-e0a9-e50e24dcca9e  NOTIFY（CCCD 自动生成）
 * 广播名：kskbl zdjd
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ble_bridge_init(void);
void      ble_bridge_start(void);
void      ble_bridge_notify(const char *text);
bool      ble_bridge_connected(void);
