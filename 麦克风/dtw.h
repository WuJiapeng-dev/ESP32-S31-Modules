/*
 * dtw.h - 动态时间规整（DTW）模板比对
 *
 * 为什么用 DTW 而不是 HMM/GMM：
 *   本模块的离线方案是"自学习小词表"——用户先教几句话，每句话存一条模板。
 *   同一句话两次说出的时长、节奏必然不同，DTW 能把时间轴对齐后再算距离，
 *   不需要训练语料，也不占 Flash 放声学模型，是最适合 MCU 的孤立词识别方法。
 */
#pragma once

#include <stdint.h>

/*
 * a / b 都是 na×dim / nb×dim 的 int16 特征序列（已做 CMN）。
 * 返回按路径长度归一化后的平均帧距离，数值越小越像；同一次录音的理想值约 0。
 */
float dtw_distance(const int16_t *a, int na, const int16_t *b, int nb, int dim);
