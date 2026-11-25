/*
 * kws.h - 离线关键词识别（两句固定短语，板载麦克风）
 *
 * 支持的两句：
 *   0 = "你好"
 *   1 = "康神开播了，真的假的"
 *
 * 两条判决路径（互为兜底，都不需要联网）：
 *   A. 声学特征分类（默认，开箱即用、不需要先教）
 *      依据是这两句话在声学上差异极大：一句双音节短促、一句八音节长句。
 *      用自适应的端点检测拿到一句话，再数能量包络的音节峰个数 + 总时长，
 *      两个条件联合作判，不是把文字写死后循环打印。
 *   B. MFCC + DTW 模板匹配（可选增强，串口发 LEARN:0 / LEARN:1 各教一遍后启用）
 *      模板存 NVS，掉电不丢。教过模板之后同一句话的 DTW 距离会明显低于阈值，
 *      判决优先走模板；没教过的句子仍然回落到 A。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define KWS_PHRASE_SHORT   0
#define KWS_PHRASE_LONG    1
#define KWS_PHRASE_COUNT   2

/* 命中回调：id 为短语编号，text 为对应中文 */
typedef void (*kws_cb_t)(int id, const char *text);

void kws_begin(kws_cb_t cb);

/* 喂入一帧 10ms 音频（160 个 16bit 样本） */
void kws_feed(const int16_t *pcm, size_t n);

/*
 * 开始录制模板：id 取 KWS_PHRASE_SHORT / KWS_PHRASE_LONG。
 * 调用后等下一句话说完就会存成模板并打印结果。
 */
void kws_enroll(int id);

/* 清除所有模板 */
void kws_clear_templates(void);

/* 当前是否已有模板（0 表示走声学分类路径） */
int kws_template_count(void);

/* 当前自适应噪声底，调试用 */
float kws_noise_floor(void);
