/*
 * tts.h - 播放编排层：缓冲 BLE 写入 -> UTF-8 转 GB2312 -> 驱动 SYN6288 -> Notify 回执
 *
 * 为什么要有缓冲：BLE 默认 ATT MTU 23 时单次写只有 20 字节，手机发一句长中文
 * 会被拆成多次 Write。这里把连续写入拼起来，静默 250ms 视为一句话结束。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TTS_TEXT_MAX        512     /* 单句 UTF-8 上限（字节） */

esp_err_t tts_init(void);

/* 由 BLE 写回调调用：只做拷贝，立即返回，不阻塞协议栈 */
void tts_feed(const uint8_t *data, size_t len);

/*
 * 直接播报一句话（不经过"静默 250ms 判定一句话"的缓冲）：
 * 识别命中后的复述、录入提示都走这里，避免提示音被 250ms 延迟拖后。
 */
void tts_say(const char *text);

/*
 * 是否正在播报。识别任务靠它暂停收音——
 * 否则 SYN6288 放出来的声音会被自己的麦克风收进去，形成自激复读。
 */
bool tts_busy(void);
