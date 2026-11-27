/*
 * dialog.h - 一轮对话的状态机（录音 -> ASR -> LLM -> TTS 播放）
 *
 * 全程不需要按键或唤醒词：能量端点检测到有人说话就开始录，
 * 说完自动上传识别，拿到回答直接播出来。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 创建对话任务（内部自己起 FreeRTOS 任务，不阻塞调用方） */
esp_err_t dialog_start(void);

/* 当前是否在「播报中」（其它模块要抢喇叭时可以先问一下） */
bool dialog_is_speaking(void);
