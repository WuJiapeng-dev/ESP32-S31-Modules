/*
 * Onboard-Microphone-Arduino.ino - ESP32-S31 板载麦克风 + BLE 双向 + 离线关键词识别（Arduino 框架）
 *
 * 硬件：慧勤智远 ESP32-S31 CB V1.1 小系统板
 *       两颗 LMA3729T381-OAS02 模拟 MEMS 麦克风 → ES8389（I2S + I2C）→ ESP32-S31
 *       不需要外接 INMP441，不需要接任何杜邦线
 *
 * 串口监视器：115200，UTF-8
 *
 * 串口里能看到的四类信息：
 *   【蓝牙】  连接/断开/GATT 就绪
 *   【手机】  手机通过 BLE 发来的文字（原样）
 *   【开发板】板子发给手机的文字（同一份内容串口也留一份）
 *   【识别】  端点检测与关键词识别过程
 *
 * 手机端：BLE 调试助手，连接 "kskbl zdjd"，订阅 6a400003 的 Notify，
 *         往 6a400002 写文本。
 */
#include <Arduino.h>
#include <string.h>
#include <strings.h>

#include "ble_bridge.h"
#include "es8389.h"
#include "kws.h"
#include "mfcc.h"
#include "mic_i2s.h"

#define APP_VER "麦克风(Arduino)/1.0"

static bool s_mic_ok   = false;
static bool s_codec_ok = false;

/* ------------------------------------------------------------------ */
/* 识别命中：串口 + 手机都要看到同一句话                                */
/* ------------------------------------------------------------------ */
static void on_phrase(int id, const char *text)
{
    (void)id;
    char msg[96];
    snprintf(msg, sizeof(msg), "识别到：%s", text);

    /* ble_send 内部会先打一行「【开发板】识别到：你好」，再发给手机 */
    ble_send(msg);
}

/* ------------------------------------------------------------------ */
/* 手机发来的文本                                                       */
/* ------------------------------------------------------------------ */
static void on_ble_text(const char *text)
{
    if (text == NULL) {
        return;
    }

    if (strcasecmp(text, "PING") == 0) {
        ble_send("PONG");
        return;
    }
    if (strcasecmp(text, "VER") == 0) {
        ble_send(APP_VER);
        return;
    }
    if (strcasecmp(text, "CLEAR") == 0) {
        kws_clear_templates();
        ble_send("OK:CLEARED");
        return;
    }
    if (strcasecmp(text, "STATE") == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "STATE:mic=%d codec=%d tmpl=%d conn=%d",
                 s_mic_ok ? 1 : 0, s_codec_ok ? 1 : 0,
                 kws_template_count(), ble_connected() ? 1 : 0);
        ble_send(buf);
        return;
    }
    if (strncasecmp(text, "LEARN:", 6) == 0) {
        const char *arg = text + 6;
        if (strcmp(arg, "0") == 0 || strstr(arg, "你好") != NULL) {
            kws_enroll(KWS_PHRASE_SHORT);
            ble_send("OK:请说「你好」");
        } else if (strcmp(arg, "1") == 0 || strstr(arg, "康神") != NULL) {
            kws_enroll(KWS_PHRASE_LONG);
            ble_send("OK:请说「康神开播了，真的假的」");
        } else {
            ble_send("ERR:LEARN参数应为 0 或 1");
        }
        return;
    }

    /* 不是命令就只回一个已收到，方便手机端确认链路是通的 */
    char echo[96];
    snprintf(echo, sizeof(echo), "已收到：%s", text);
    ble_send(echo);
}

/* ------------------------------------------------------------------ */
/* 串口本地命令（不影响 BLE 功能，调试用）                              */
/* ------------------------------------------------------------------ */
static void handle_serial_line(const char *line)
{
    if (strcasecmp(line, "lvl") == 0) {
        mic_level_check();
    } else if (strcasecmp(line, "l0") == 0) {
        kws_enroll(KWS_PHRASE_SHORT);
    } else if (strcasecmp(line, "l1") == 0) {
        kws_enroll(KWS_PHRASE_LONG);
    } else if (strcasecmp(line, "clr") == 0) {
        kws_clear_templates();
    } else if (strcasecmp(line, "state") == 0) {
        Serial.printf("【状态】mic=%d codec=%d 模板=%d 手机连接=%d 噪声底=%.0f\r\n",
                      s_mic_ok ? 1 : 0, s_codec_ok ? 1 : 0,
                      kws_template_count(), ble_connected() ? 1 : 0,
                      kws_noise_floor());
    } else if (line[0] != '\0') {
        Serial.printf("【串口】未知命令：%s\r\n", line);
        Serial.println("【串口】可用：lvl=电平自检 l0=教「你好」 l1=教长句 clr=清模板 state=状态");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(400);                     /* 等 USB 串口枚举完成，否则开头几行会丢 */

    Serial.println();
    Serial.println("==== ESP32-S31 板载麦克风 + BLE 双向 + 离线关键词识别 ====");
    Serial.println("固件：麦克风(Arduino)/1.0   串口：115200 UTF-8");

    /* 必须先让 I2S 跑起来：ES8389 用 use_mclk=false，内部时钟来自 BCLK，
       没有 BCLK 时配置采样率分频不会生效 */
    s_mic_ok = mic_begin();

    if (s_mic_ok) {
        s_codec_ok = es8389_begin(MIC_I2C_SDA_GPIO, MIC_I2C_SCL_GPIO);
        if (s_codec_ok) {
            es8389_set_mic_gain_db(MIC_PGA_DB);
            delay(300);             /* 等 ADC 与 PGA 稳定再取电平 */
            mic_level_check();
        }
    } else {
        Serial.println("【错误】I2S 启动失败，后面不会采到声音");
    }

    kws_begin(on_phrase);
    ble_begin(on_ble_text);

    Serial.printf("【状态】可用堆内存 %u 字节\r\n", (unsigned)ESP.getFreeHeap());
    Serial.println("【提示】对着板子说「你好」或「康神开播了，真的假的」");
    Serial.println("【提示】想要更稳可以对每句先教一遍：串口发 l0 / l1，或手机发 LEARN:0 / LEARN:1");
}

void loop()
{
    /* 每 10ms 取 160 个样本；mic_read 内部有超时，拿不到就下一轮 */
    int16_t blk[MFCC_HOP];

    size_t got = mic_read(blk, MFCC_HOP, 100);
    if (got == 0) {
        delay(1);
    } else {
        if (got < MFCC_HOP) {
            memset(blk + got, 0, (MFCC_HOP - got) * sizeof(int16_t));
        }
        kws_feed(blk, MFCC_HOP);
    }

    /* 串口本地命令，逐行处理 */
    static char   line[96];
    static size_t line_len = 0;

    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) {
            break;
        }
        if (c == '\r' || c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                handle_serial_line(line);
                line_len = 0;
            }
        } else if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }
}
