/*
 * volc_tts.h - 火山引擎（豆包）语音合成 HTTP 客户端
 *
 * 接口来源（官方文档）：
 *   大模型 HTTP 非流式接口-V1
 *   https://www.volcengine.cn/docs/DoubaoVoice/HTTPone-timecompositenon-streaminginterface-V1notrecommended
 *   POST https://openspeech.bytedance.com/api/v1/tts
 *   Header: "Authorization: Bearer;${token}"   ← 注意 Bearer 与 token 之间是分号，不是空格
 *   鉴权说明：https://docs.volcengine.com/docs/DoubaoVoice/Authenticationmethod
 *
 * 返回体是 JSON，音频经 base64 放在 data 字段：{"code":3000,...,"data":"<base64>"}
 *
 * 本实现的关键取舍：
 *   不把整段音频读进内存再解码。V1 是一次性返回，一段 5 秒 16kHz 16bit PCM 约 160KB，
 *   base64 后 213KB，而本工程没开 PSRAM（CONFIG_SPIRAM is not set），SRAM 撑不住。
 *   所以这里流式扫描响应体：先确认 "code":3000，再定位 "data":"，之后逐字符增量
 *   base64 解码，解出多少 PCM 就立刻交给回调去推 I2S，峰值内存只有几 KB。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "cfg_store.h"

/* PCM 数据回调：samples 为单声道 16bit 样本个数 */
typedef esp_err_t (*volc_pcm_cb_t)(const int16_t *pcm, size_t samples, void *user);

/* 合成结果概要，用于上报 */
typedef struct {
    uint32_t pcm_bytes;     /* 本次解出的 PCM 总字节数 */
    uint32_t elapsed_ms;    /* 从发出请求到收完的耗时 */
    int      http_status;
    int      api_code;      /* 火山业务码，3000 为成功 */
} volc_result_t;

/*
 * 合成一段文本并流式播放。
 * text 为 UTF-8，建议单次不超过 300 汉字（火山 V1 上限 1024 UTF-8 字节）。
 * on_pcm 可为 NULL（只测通不通，不出声）。
 */
esp_err_t volc_tts_speak(const app_cfg_t *cfg,
                         const char *text,
                         volc_pcm_cb_t on_pcm,
                         void *user,
                         volc_result_t *result);
