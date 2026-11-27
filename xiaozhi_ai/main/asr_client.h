/*
 * asr_client.h - DeepSeek 语音识别（/v1/tools/asr）客户端
 *
 * 一次请求发送整段音频，返回整段文本，不支持流式（官方文档明确说明）。
 * 音频按官方要求封成 16bit PCM WAV，单声道 16kHz。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cfg_store.h"

/* 转写结果最大字节数（一句话一般几十字节，留足余量） */
#define ASR_TEXT_MAX    512

/*
 * 把 pcm（samples 个 16bit 单声道样本）转写为文字。
 * 成功时 out_text 是 UTF-8 文本，同时打印【识别】日志。
 */
esp_err_t asr_client_transcribe(const app_cfg_t *cfg,
                                const int16_t *pcm,
                                size_t samples,
                                char *out_text,
                                size_t out_size);
