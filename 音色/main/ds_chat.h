#pragma once

#include <stddef.h>
#include "esp_err.h"

/*
 * DeepSeek 对话（chat/completions）最小实现。
 *
 * 只做一件事：把一句话发给 api.deepseek.com/chat/completions，
 * 拿回纯文本回答。单轮对话，不保存上下文。
 *
 * 注意：内部走 HTTPS + TLS，调用方任务栈必须 >= 12288 字节。
 */

esp_err_t ds_chat_ask(const char *api_key, const char *question,
                      char *reply, size_t reply_size, int *http_status);

/* JSON 字符串转义（不含首尾引号）；返回写入长度，-1 表示缓冲不够 */
int ds_chat_json_escape(const char *src, char *dst, int cap);
