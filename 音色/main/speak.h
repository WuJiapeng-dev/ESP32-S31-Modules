/*
 * speak.h - 播放调度层：收文本 -> 按标点切块 -> 逐块走 DeepSeek TTS -> 推 I2S
 *
 * 为什么要切块：
 *   DeepSeek 的 TTS 是一边合成一边往回吐音频的，本身不需要按句切开。
 *   但单次请求有 10000 计费字符 / 64 KiB 上限，且一次失败会丢掉整段，
 *   所以按 1024 字节切块，切点优先落在标点上；正常一两句话只会切成一块。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t speak_init(void);

/* BLE 写特征每收到一段就喂进来；内部累积到静默 300ms 视为一句说完 */
void speak_feed(const uint8_t *data, size_t len);

/* 打断当前播放（已发出的 HTTP 请求会自然跑完，但后续句子不再处理） */
void speak_stop(void);

/* 立刻合成并播报一句（控制台「试听」用），不等静默窗口 */
esp_err_t speak_say(const char *text);

/* 控制台改完配置后调用，让播放层用新参数（不用重启） */
void speak_reload_cfg(void);

bool speak_busy(void);
