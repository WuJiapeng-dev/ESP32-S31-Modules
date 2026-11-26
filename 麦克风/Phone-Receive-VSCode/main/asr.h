/*
 * asr.h - 离线识别 + BLE 命令入口（手机接收版：不接喇叭）
 *
 * 方案（可行性结论见 README §1）：
 *   ESP32-S31 上跑不了通用中文大词表 ASR（没有 esp-sr 的 s31 预编译库，
 *   whisper/vosk 模型体积远超 RAM）。这里用**自学习模板匹配**：
 *   用户先用 EN:<标签> 教句子，之后说同样的话，MFCC+DTW 比对，够近就输出该标签。
 *   它认的是教过的句子，不是任意中文；没命中明确回 ASR:?，不编造结果。
 *
 * BLE 命令（写 6a400002，UTF-8 文本，静默 250ms 视为一条命令结束）：
 *   ASR | ASR:ON    开始识别
 *   ASR:OFF | STOP  停止识别
 *   EN:<标签>        下一条语音存成该标签的模板（标签可中文，≤39 字节）
 *   LIST            列出模板
 *   DEL:<序号或标签>  删除
 *   CLEAR           清空全部
 *   TH:<数值>        判定阈值（默认 18.0，越小越严格）
 *   STATE           查询当前状态
 *   PING / VER      连通性 / 版本
 *
 * Notify（6a400003）回报：
 *   PONG / OK:ASRON n / OK:ASROFF / OK:ENROLLING <标签> / OK:ENROLLED <标签>
 *   ASR:<标签> 命中；SCORE:<d> 距离；ASR:? d=.. th=.. 未命中
 *   LIST:n / T<i>:<标签> / OK:DEL / OK:CLEAR / OK:TH / STATE:...
 *   ERR:NOSUCH / ERR:FULL / ERR:RANGE / ERR:UNKNOWN / ERR:SAVE
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t asr_init(void);

/* 由 BLE 写回调调用：只拷贝，立即返回 */
void app_cmd_feed(const uint8_t *data, size_t len);
