/*
 * ds_tts.h - DeepSeek 语音合成 HTTP 客户端
 *
 * 接口来源（官方文档，2026-11 核对）：
 *   https://api-docs.deepseek.com/zh-cn/guides/tts/
 *   POST https://api.deepseek.com/v1/tools/tts
 *   Header: Authorization: Bearer <API_KEY>      ← 标准写法，空格分隔
 *   Body:   {"text":"...","voice_id":"stella","format":"pcm"}
 *
 * 与火山引擎那套的关键差别（这也是改这一版的原因）：
 *   1. 响应体就是音频裸流，不是 base64 包在 JSON 里 —— 不需要状态机扫 JSON，
 *      也不需要增量 base64 解码，读到字节直接拼样本推 I2S。
 *   2. 只认一个 API Key，没有 AppID / Token / cluster 三件套。
 *   3. 音频边合成边返回，一次请求就能连续出声，不用按句切开逐句请求。
 *   4. 代价：只有两个音色（stella 女声 / tide 男声），且**不支持调语速和音量**。
 *
 * 输出格式固定为 format=pcm：
 *   24 kHz、16 bit、单声道、有符号小端裸 PCM（官方明确写明）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cfg_store.h"

/* 单次请求的文本上限（字节）。官方上限是 10000 计费字符 / 64 KiB 请求体，
 * 这里取 1024 字节是为了控制单次失败的影响面，实际约 340 个汉字。 */
#define DS_TEXT_MAX         1024

/* PCM 数据回调：samples 为单声道 16bit 样本个数 */
typedef esp_err_t (*ds_pcm_cb_t)(const int16_t *pcm, size_t samples, void *user);

/* 合成结果概要，用于日志与状态上报 */
typedef struct {
    uint32_t pcm_bytes;     /* 本次收到的 PCM 总字节数 */
    uint32_t elapsed_ms;    /* 从发出请求到收完的耗时 */
    int      http_status;   /* HTTP 状态码，200 为正常 */
} ds_result_t;

/*
 * 合成一段文本并边收边播。
 * text 为 UTF-8，不超过 DS_TEXT_MAX 字节。
 * on_pcm 可为 NULL（只测通不通，不出声）。
 */
esp_err_t ds_tts_speak(const app_cfg_t *cfg,
                       const char *text,
                       ds_pcm_cb_t on_pcm,
                       void *user,
                       ds_result_t *result);
