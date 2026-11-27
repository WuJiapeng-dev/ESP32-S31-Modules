#include "log_ring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define LOG_LINES   80
#define LOG_CHAR    112
#define FMT_BUF     200

static char           s_buf[LOG_LINES][LOG_CHAR];
static uint32_t       s_seq[LOG_LINES];
static uint32_t       s_head;        /* 下一个写入位置 */
static uint32_t       s_count;       /* 已存行数（<= LOG_LINES） */
static uint32_t       s_next = 1;    /* 下一行的序号，从 1 开始，0 表示“从头” */
static vprintf_like_t s_orig;
static portMUX_TYPE   s_lock = portMUX_INITIALIZER_UNLOCKED;

/* 接管日志输出：先存一份，再照原样输出到串口 */
static int log_vprintf(const char *fmt, va_list ap)
{
    char tmp[FMT_BUF];
    va_list cp;

    va_copy(cp, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, cp);
    va_end(cp);

    if (n > 0) {
        int len = (n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp) - 1;

        /* 末尾换行去掉；中间的换行压成空格，一条日志当一行存 */
        while (len > 0 && (tmp[len - 1] == '\n' || tmp[len - 1] == '\r')) {
            len--;
        }
        for (int i = 0; i < len; i++) {
            if (tmp[i] == '\n' || tmp[i] == '\r') {
                tmp[i] = ' ';
            }
        }
        tmp[len] = '\0';

        if (len > 0) {
            taskENTER_CRITICAL(&s_lock);
            uint32_t i = s_head;
            strlcpy(s_buf[i], tmp, LOG_CHAR);
            s_seq[i] = s_next++;
            s_head = (s_head + 1u) % LOG_LINES;
            if (s_count < LOG_LINES) {
                s_count++;
            }
            taskEXIT_CRITICAL(&s_lock);
        }
    }

    return (s_orig != NULL) ? s_orig(fmt, ap) : n;
}

void log_ring_init(void)
{
    if (s_orig == NULL) {
        s_orig = esp_log_set_vprintf(log_vprintf);
    }
}

uint32_t log_ring_next(void)
{
    uint32_t v;
    taskENTER_CRITICAL(&s_lock);
    v = s_next;
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

int log_ring_drain(uint32_t since, char *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return 0;
    }
    out[0] = '\0';

    size_t used = 0;

    taskENTER_CRITICAL(&s_lock);
    uint32_t start = (s_head + LOG_LINES - s_count) % LOG_LINES;

    for (uint32_t k = 0; k < s_count; k++) {
        uint32_t idx = (start + k) % LOG_LINES;
        if (s_seq[idx] < since) {
            continue;
        }

        size_t n = strlen(s_buf[idx]);
        if (used + n + 2 > cap) {
            break;
        }
        memcpy(out + used, s_buf[idx], n);
        used += n;
        out[used++] = '\n';
    }
    taskEXIT_CRITICAL(&s_lock);

    out[used] = '\0';
    return (int)used;
}

void log_ring_clear(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_count = 0;
    s_head  = 0;
    taskEXIT_CRITICAL(&s_lock);
}
