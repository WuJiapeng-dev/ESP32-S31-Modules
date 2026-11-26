/*
 * asr.h - 离线识别编排 + BLE 命令入口
 *
 * 方案说明（重要）：
 *   本工程**不跑通用中文大词表 ASR**（可行性结论见 README）：
 *   esp-sr / whisper / vosk 这类方案要么没有 esp32s31 的预编译库，
 *   要么模型体积（几十 MB）远超芯片 RAM。这里用的是
 *   "自学习模板匹配"：用户先教几句话（每句存一条 MFCC 模板），
 *   之后说同样的话，用 DTW 对齐后算距离，距离够小就判定为那句话。
 *
 *   所以它认的是"教过的句子"，不是任意中文。这一点在 README 和汇报里都写明。
 *
 * BLE 命令（写入 6a400002 特征，UTF-8 文本，静默 250ms 视为一条命令结束）：
 *   ASR            打开识别（默认关闭，避免误触发）
 *   ASR:OFF/STOP   关闭识别
 *   EN:<标签>       下一条语音作为 <标签> 的模板存入（标签可中文，最长 39 字节）
 *   LIST           列出已存模板
 *   DEL:<序号或标签> 删除一条模板
 *   CLEAR          清空全部模板
 *   TH:<数值>       设置判定阈值（默认 18.0，越小越严格）
 *   SAY:<文本>      直接播报（不经识别）
 *   PING           连通性测试
 *   其他文本        兼容上一个模块：直接交给 SYN6288 播报
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t asr_init(void);

/* 由 BLE 写回调调用：只拷贝，立即返回 */
void app_cmd_feed(const uint8_t *data, size_t len);
