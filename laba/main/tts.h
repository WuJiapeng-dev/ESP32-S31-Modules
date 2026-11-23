/*
 * tts.h - 播放编排层：缓冲 BLE 写入 -> UTF-8 转 GB2312 -> 驱动 SYN6288 -> Notify 回执
 *
 * 为什么要有缓冲：BLE 默认 ATT MTU 23 时单次写只有 20 字节，手机发一句长中文
 * 会被拆成多次 Write。这里把连续写入拼起来，静默 250ms 视为一句话结束。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TTS_TEXT_MAX        512     /* 单句 UTF-8 上限（字节） */

esp_err_t tts_init(void);

/* 由 BLE 写回调调用：只做拷贝，立即返回，不阻塞协议栈 */
void tts_feed(const uint8_t *data, size_t len);
