#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 把 ESP_LOG 的输出同时留一份在内存里，供网页拉取。
 *
 * 做法：接管 esp_log_set_vprintf，格式化后先塞环形缓冲，再交回原来的
 * 输出函数（串口照旧能看，互不影响）。
 *
 * 只保留最近 80 行、每行最多 111 字节，够定位问题，也不吃太多 RAM。
 */

void log_ring_init(void);

/* 最新序号 + 1（客户端下次把这个值当 since 传回来） */
uint32_t log_ring_next(void);

/*
 * 取序号 >= since 的日志，按行写进 out（每行以 '\n' 结尾，末尾 '\0'）。
 * 写满 cap 就停。返回写入字节数。
 */
int log_ring_drain(uint32_t since, char *out, size_t cap);

void log_ring_clear(void);
