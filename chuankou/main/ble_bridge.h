/*
 * ble_bridge.h - BLE（NimBLE）对外接口
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 注册 GATT 服务 + 设置广播名，必须在 nimble_port_init() 之后调用 */
esp_err_t ble_bridge_init(void);

/* 协议栈同步（sync）后开始广播 */
void ble_bridge_start(void);

/* 通过 Notify 特征把执行结果回传给手机 */
void ble_bridge_notify(const char *text);

/* 当前是否有 BLE 连接 */
bool ble_bridge_connected(void);
