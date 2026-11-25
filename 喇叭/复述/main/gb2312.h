/*
 * gb2312.h - UTF-8 -> GB2312 转码（无 iconv 依赖，自带码表二分查找）
 *
 * 为什么需要：SYN6288 只接受 GB2312/GBK 编码的文本，手机 BLE 写入的是 UTF-8。
 * ESP-IDF 的 newlib 没启用 iconv，没有现成的转码器，所以把 GB2312 码表编进固件。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 把 UTF-8 字符串转为 GB2312 字节流。
 *   out      : 输出缓冲区
 *   out_cap  : 输出缓冲区容量（字节）
 *   skipped  : 无法映射的字符个数（GB2312 之外的生僻字/emoji），可为 NULL
 * 返回实际写入 out 的字节数；遇到缓冲区不足时停止并计入 skipped。
 */
size_t gb2312_from_utf8(const char *in, uint8_t *out, size_t out_cap, size_t *skipped);

/* 码表信息（供启动日志自检） */
size_t   gb2312_table_size(void);
