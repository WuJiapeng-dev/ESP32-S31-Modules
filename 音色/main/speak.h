/*
 * speak.h - 播放调度层：收文本 -> 分句 -> 逐句走云端 TTS -> 推 I2S
 *
 * 为什么必须分句：
 *   火山 V1 是非流式接口，整段文本合成完才一次性返回。若把一整段直接丢过去，
 *   一段 10 秒的话要等 10 秒才出声。按标点切成短句后，第一句只要几百毫秒就能开口。
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
