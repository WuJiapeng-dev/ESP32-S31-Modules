/*
 * app_config.h - 全局参数集中定义（引脚见 mic_i2s.h / audio_out.h）
 *
 * DeepSeek 三个接口的出处（官方文档，2026-11-27 核对）：
 *   ASR: https://api-docs.deepseek.com/zh-cn/guides/asr/
 *        POST /v1/tools/asr ，multipart/form-data 的 file 字段，
 *        接受 16bit PCM WAV / Ogg Opus，WAV 支持 8000~48000Hz，单声道，
 *        单次最长 120 秒、最大 8MiB，返回 {"text":"..."}
 *   LLM: https://api-docs.deepseek.com/zh-cn/
 *        POST /chat/completions ，OpenAI 兼容，model=deepseek-flash
 *   TTS: https://api-docs.deepseek.com/zh-cn/guides/tts/
 *        POST /v1/tools/tts ，{"text","voice_id","format":"pcm"}，
 *        format=pcm 返回 24kHz/16bit/单声道裸流
 */
#pragma once

#define APP_VER                 "小智ai接入/1.0"

/* ---------------- DeepSeek 接口 ---------------- */
#define DS_API_BASE             "https://api.deepseek.com"
#define DS_ASR_URL              DS_API_BASE "/v1/tools/asr"
#define DS_CHAT_URL             DS_API_BASE "/chat/completions"
#define DS_TTS_URL              DS_API_BASE "/v1/tools/tts"

/* 模型名按官方文档：deepseek-flash（旧名 deepseek-v4-flash 仍可调用） */
#define DS_LLM_MODEL            "deepseek-flash"

/* 单次回答最多 token，控制延迟与费用 */
#define DS_LLM_MAX_TOKENS       320

/* 系统提示词：让它说人话、别用 Markdown（TTS 会把符号念出来） */
#define DS_SYSTEM_PROMPT        "你是一个装在 ESP32 开发板上的中文语音助手。" \
                                "回答要口语化、简短，控制在 80 字以内。" \
                                "不要使用任何 Markdown 标记、列表符号或表情符号，" \
                                "因为你的回答会被直接朗读出来。"

/* ---------------- 录音与端点检测 ---------------- */
#define DIALOG_MAX_RECORD_MS    8000    /* 单句最长 8 秒（DeepSeek ASR 上限 120s，这里按对话习惯收紧） */
#define DIALOG_SILENCE_MS       700     /* 说完后静音多久算一句结束 */
#define DIALOG_MIN_VOICE_MS     400     /* 有效语音短于这个时长直接丢弃，避免噪声触发 */
#define DIALOG_PREROLL_MS       200     /* 触发前的预滚，防吃字头 */

/* 录音缓冲：8 秒 × 16kHz × 2 字节 = 256KB，放 PSRAM */
#define DIALOG_BUF_BYTES        (MIC_SAMPLE_RATE * 2 * DIALOG_MAX_RECORD_MS / 1000)

/* ---------------- 一轮对话的历史保留 ---------------- */
/* 只在内存里保留最近 3 轮，避免长会话把 token 烧光 */
#define DS_HISTORY_TURNS        3
