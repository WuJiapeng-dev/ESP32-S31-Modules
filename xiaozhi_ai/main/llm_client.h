/*
 * llm_client.h - DeepSeek 对话（/chat/completions）客户端
 *
 * 只保留最近 DS_HISTORY_TURNS 轮上下文，够连续对话，又不会把 token 烧光。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "cfg_store.h"

#define LLM_REPLY_MAX   1024

/*
 * 发一句话，拿回回答（UTF-8）。
 * 成功时打印【AI】日志。
 */
esp_err_t llm_client_chat(const app_cfg_t *cfg,
                          const char *user_text,
                          char *out_reply,
                          size_t out_size);

/* 清空对话历史（重新开始一段对话时调用） */
void llm_client_reset(void);
