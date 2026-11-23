/*
 * gb2312.c - UTF-8 -> GB2312 转码实现
 *
 * 码表由 tools/gen_gb2312_table.py 生成（g_gb2312_map），
 * 每项 (unicode << 16) | gb2312 双字节，按 unicode 升序，二分查找。
 */
#include "gb2312.h"

#include <stdbool.h>

extern const uint32_t g_gb2312_map[];
extern const size_t   g_gb2312_map_len;

size_t gb2312_table_size(void)
{
    return g_gb2312_map_len;
}

static uint16_t lookup_gb2312(uint32_t cp)
{
    size_t lo = 0;
    size_t hi = g_gb2312_map_len;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t key = g_gb2312_map[mid] >> 16;

        if (key == cp) {
            return (uint16_t)(g_gb2312_map[mid] & 0xFFFF);
        }
        if (key < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

size_t gb2312_from_utf8(const char *in, uint8_t *out, size_t out_cap, size_t *skipped)
{
    size_t oi = 0;
    size_t sk = 0;
    const uint8_t *p = (const uint8_t *)in;

    if (in == NULL || out == NULL) {
        if (skipped != NULL) {
            *skipped = 0;
        }
        return 0;
    }

    while (*p != '\0') {
        uint32_t cp;
        int extra;

        if (*p < 0x80) {
            cp = *p;
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            extra = 3;
        } else {
            p++;                    /* 非法 UTF-8 起始字节 */
            sk++;
            continue;
        }

        bool valid = true;
        for (int i = 1; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) {
            p++;                    /* 截断的序列，丢一个字节继续 */
            sk++;
            continue;
        }
        p += (size_t)(1 + extra);

        /* 控制字符换成空格：BLE 助手的发送框常带 \r\n，直接送进 TTS 会粘连 */
        if (cp < 0x20 || cp == 0x7F) {
            cp = ' ';
        }

        if (cp < 0x80) {
            if (oi + 1 <= out_cap) {
                out[oi++] = (uint8_t)cp;
            } else {
                sk++;
            }
        } else {
            uint16_t g = lookup_gb2312(cp);
            if (g != 0 && oi + 2 <= out_cap) {
                out[oi++] = (uint8_t)(g >> 8);
                out[oi++] = (uint8_t)(g & 0xFF);
            } else {
                sk++;               /* GB2312 之外（生僻字/emoji）→ 跳过并计数 */
            }
        }
    }

    if (skipped != NULL) {
        *skipped = sk;
    }
    return oi;
}
