# laba_cloud — ESP32-S31 云端语音合成（BLE → 火山引擎 TTS → I2S）

手机 BLE 连 `kskbl zdjd`，往写特征发中文，ESP32-S31 走 HTTPS 调火山引擎语音合成，
拿回 PCM 直接推 I2S 给数字功放出声。**音色、语速、音量都能在网页控制台里改**。

本目录是 **ESP-IDF 工程根**，与仓库里 `chuankou/`、`laba/` 并列，不是仓库根目录的主工程。

---

## 1. 为什么另起一个模块

`laba/` 用的是 SYN6288，那颗芯片是**喂文本、吐模拟声**的封闭器件：

- 输入只有 UART 文本，音频由芯片内部 DAC 直接推功放（手册 §1.7 引脚 13 `BP0` / 15 `BN0`）
- 音色固化了**一个**，手册全篇没有任何切换发音人的命令或标记
- ESP32 碰不到音频数据，所以「软件变调换音色」这条路物理上行不通

要换音色只有走云端 TTS，而云端返回的是**数字音频流**，SYN6288 没有音频输入口。
所以这不是"改个配置"，是整条播放链路换掉——因此单独成模块，`laba/` 原样保留当离线兜底。

| | laba（SYN6288） | laba_cloud（本模块） |
|---|---|---|
| 出声路径 | UART 文本 → 芯片内部合成 | HTTPS 取 PCM → I2S → 功放 |
| 音色 | 固定一个 | 火山音色库随便挑 |
| 语速 | `[t0]`~`[t5]` 六档 | 0.5~2.0 连续可调 |
| 联网 | 不需要 | 必须 |
| 占用引脚 | UART1: GPIO8/9/12 | I2S: GPIO15/16/17 |

---

## 2. 链路

```
手机 BLE 写特征 (6a400002…)
   ↓ speak_feed()      累积，静默 300ms 视为一句说完
   ↓ split_sentence()  按标点切短句
   ↓ volc_tts_speak()  POST https://openspeech.bytedance.com/api/v1/tts
   ↓ 流式扫 JSON + 增量 base64 解码 → int16 PCM
   ↓ audio_out_write_mono16()  单声道复制成双声道
   ↓ I2S0 @16kHz 16bit
数字功放（MAX98357A / PCM5102 类）
   ↓
喇叭
```

状态经 Notify 特征回传：`OK:PLAY` / `OK:NEXT` / `OK:DONE` / `OK:STOP` / `ERR:NET` / `ERR:API` / `ERR:NOWIFI`。

---

## 3. 接线

### 3.1 I2S 功放

| ESP32-S31 | 方向 | 功放模块 | 说明 |
|---|---|---|---|
| GPIO15 | → | BCLK | 位时钟 |
| GPIO16 | → | LRC / WS | 帧时钟，有的模块标 LRCLK |
| GPIO17 | → | DIN / SD | 音频数据 |
| 3V3 | → | VIN | MAX98357A 可接 5V 得更大功率 |
| GND | → | GND | 必须共地 |

- 不需要 MCLK。MAX98357A、PCM5102、UDA1334 这类都不需要，代码里 `mclk = I2S_GPIO_UNUSED`。
- GPIO15/16/17 均在 `soc_caps.h` 确认有效：`SOC_GPIO_VALID_GPIO_MASK` 只排除了 **GPIO29 与 GPIO41**（注释写明 *not bonded*，未键合）。
- 与 `chuankou`（UART0: GPIO58/59）、`laba`（UART1: GPIO8/9/12）、USB PHY（GPIO33/34）**完全不冲突**，三个模块的线可以同时接着。

### 3.2 关于功放增益

MAX98357A 板上有个 GAIN 焊盘/电阻，默认 9dB。嫌声音小就按模块丝印改到 12/15dB，
比在软件里拉 `volume_ratio` 更干净（后者只是数字增益，拉满会削波）。

---

## 4. 配网与控制台

模块跑 **APSTA**：热点常开，同时尝试连路由器。

1. 手机连热点 **`kskbl-setup`**（开放的，无需密码）
2. 浏览器打开 **http://192.168.4.1/**
3. 四个页签：
   - **连接**：填 Wi-Fi 名称和密码（只支持 2.4GHz，5GHz 连不上）
   - **引擎**：填火山 AppID / Access Token / cluster
   - **音色**：填 `voice_type`，调语速和音量
   - **试听**：输入一句话点播放，直接听效果；下面还有设备状态表和恢复出厂

为什么热点常开：路由器换密码、搬家、改了 SSID 之后，STA 连不上时设备不会失联，
手机连上热点就能改配置——比"配错就成砖"友好得多。

配置全部存 NVS（namespace `lcfg`），掉电不丢。**源码和仓库里不出现任何密钥。**

---

## 5. 云端接口（火山引擎）

来源：[大模型 HTTP 非流式接口-V1](https://www.volcengine.cn/docs/DoubaoVoice/HTTPone-timecompositenon-streaminginterface-V1notrecommended)、[鉴权方法](https://docs.volcengine.com/docs/DoubaoVoice/Authenticationmethod)

```
POST https://openspeech.bytedance.com/api/v1/tts
Authorization: Bearer; {access_token}      ← 分号分隔，不是空格
Content-Type: application/json
```

```json
{
  "app":     {"appid":"...", "token":"...", "cluster":"volcano_tts"},
  "user":    {"uid":"esp32s31"},
  "audio":   {"voice_type":"BV700_streaming", "encoding":"pcm", "rate":16000,
              "speed_ratio":1.0, "volume_ratio":1.0},
  "request": {"reqid":"<32位随机hex>", "text":"...", "text_type":"plain", "operation":"query"}
}
```

返回体是 JSON，音频在 `data` 字段里，base64 编码：

```json
{"code":3000, "message":"Success", "data":"<base64>", "addition":{"duration":"..."}}
```

几个关键约束（都写进代码里了）：

- `audio.encoding` 取 `pcm`，`audio.rate` 取 `16000`，输出就是 16bit 小端单声道裸 PCM，正好能直接推 I2S，**不用 MP3 解码器**
- `request.reqid` 每次必须唯一，代码用 4 个 `esp_random()` 拼 32 位 hex
- `text` 上限 1024 UTF-8 字节
- **V1 不支持大模型音色**（`zh_female_*_bigtts` 这类），填了会返回错误。要那些音色得上 V3 接口
- 业务码 `code == 3000` 才算成功

---

## 6. 两个刻意的设计取舍

### 6.1 不用 cJSON，改成流式扫 JSON

V1 是**非流式**接口：整段文本合成完，一次性把整段音频 base64 塞进 JSON 返回。
一段 5 秒的 16kHz 16bit PCM 约 160 KB，base64 后 213 KB。

而本工程 `CONFIG_SPIRAM` 是关的（`# CONFIG_SPIRAM is not set`），SRAM 只有几百 KB，
Wi-Fi + TLS 还要吃掉一大半。用 cJSON 就得先把整个响应体读进内存再 parse，必 OOM。

所以 `volc_tts.c` 用状态机流式扫描：

```
找 "code":  → 确认 3000
找 "data":" → 进入 base64 段
之后逐字符 base64 解码（4 字符 → 3 字节），解出多少 PCM 就立刻写 I2S
```

峰值内存只有几 KB（读缓冲 1 KB + PCM 块 1 KB + 请求体 2 KB）。

### 6.2 必须分句

V1 不流式，整段丢过去就得等整段合成完才出声——10 秒的话要等 10 秒。
所以按标点切成短句逐句请求，第一句几百毫秒就能开口。

切句在 `speak.c::split_sentence()`，规则：
- 认中英文标点（`。，！？；、：` 和 `.!?;:`）
- 片段短于 12 字节不单独成句，避免碎成语气词
- 单句硬上限 220 字节，且**只在 UTF-8 字符边界切**，不会把一个汉字劈成两半

代价是句间有约 200~500 ms 的请求空档。想消掉得做下一句预取，目前没做。

---

## 7. 编译与烧录

### 7.1 一键

```bat
flash.bat                  :: 自动找端口 + 编译 + 烧录
flash.bat -Port COM8
flash.bat -Monitor
```

### 7.2 手动

必须在**已激活 IDF v6.1.0 的终端**里，且 `esp32s31` 是 preview 目标：

```bat
cd /d "C:\Users\yyn\Desktop\ESP32-S31  ks\laba_cloud"
idf.py --preview set-target esp32s31
set IDF_PY_BUILD_JOBS=3
idf.py --preview build
idf.py --preview -p COM8 flash
```

- `-j` 在本机非法，并行度只能用环境变量 `IDF_PY_BUILD_JOBS`（8GB 内存建议 3）
- 烧录口：板载 CH343（VID 1A86 / PID 55D3）
- **COM8 上看不到任何日志**：console 被让位给 USB Serial/JTAG
  （`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`、`CONFIG_ESP_CONSOLE_UART_NUM=-1`）。
  要看日志请插板子的**原生 USB 口**，会出现一个 Espressif CDC 端口。

### 7.3 本机编译踩到的两个坑

**坑一：默认分区装不下。** 同时编进 Wi-Fi + NimBLE + mbedTLS 根证书包后，app 二进制
`0x1a3b70`（1.64 MB），而默认 `SINGLE_APP_LARGE` 的 factory 只有 `0x177000`（1.47 MB），
`check_sizes.py` 直接报 `app partition is too small ... overflow 0x2cb70`。
已改用 `partitions.csv` 自定义，factory 给到 **4 MB**：

```
nvs        data nvs      0x9000   0x6000
phy_init   data phy      0xf000   0x1000
factory    app  factory  0x10000  0x400000
```

**坑二：bootloader 子工程与主构建并发时卡死。** 全量编译会在主构建跑到一半时
另起一套 `cmake --build .` 编 bootloader，两套 ninja 争抢文件后双双挂起，
表现为日志长时间零输出、gcc 进程 CPU 归零。**解法是先单独编一次 bootloader，再编主工程**：

```bat
idf.py --preview bootloader
idf.py --preview build
```

### 7.4 编译实测（2026-11-23）

| 项 | 结果 |
|---|---|
| app 二进制 | `0x1a3b70` = 1,719,152 字节（1.64 MB） |
| app 分区 | `0x400000`（4 MB），剩余 `0x25c490`（59%） |
| bootloader | `0x4cc0`（19.6 KB），剩余 20% |
| 编译告警 | 0 |
| 分区表 | 已验证二进制：nvs `0x9000/0x6000`、phy_init `0xf000/0x1000`、factory `0x10000/0x400000` |

---

## 8. 验收

1. 手机连 `kskbl-setup`，开 http://192.168.4.1/
2. 「连接」页填 Wi-Fi，保存 → 徽章变绿，状态表里出现 IP
3. 「引擎」页填 AppID 和 Token，保存 → 「引擎 就绪」
4. 「试听」页点播放 → 喇叭出声
5. 手机 BLE 助手连 `kskbl zdjd`，订阅 `6a400003…` 的 CCCD
6. 往 `6a400002…` 写中文，例如 `你好，我是语音合成模块`
7. 预期：喇叭播报，Notify 依次收到 `OK:PLAY` → `OK:DONE`

---

## 9. 已知限制

- **V1 非流式**，首句延迟取决于云端合成速度，实测几百毫秒；长句更慢
- **不支持大模型音色**，只能用 BV 系列；要 `zh_female_*_bigtts` 得改用 V3 WebSocket 接口
- **句间有停顿**，没做预取
- **未开 PSRAM**，靠流式解码压内存；若以后要缓存整段音频或做预取，得先确认板子有没有 PSRAM 并打开 `CONFIG_SPIRAM`
- **未实机验证**：编译已通过（0 error / 0 warning），但云端密钥、I2S 功放出声、配网流程都还需要接上实物跑一遍

---

## 10. 出处与说明

| 内容 | 来源 |
|---|---|
| TTS 接口、参数、鉴权 | [火山官方文档 V1](https://www.volcengine.cn/docs/DoubaoVoice/HTTPone-timecompositenon-streaminginterface-V1notrecommended) / [鉴权方法](https://docs.volcengine.com/docs/DoubaoVoice/Authenticationmethod) |
| BLE 服务与 UUID | 沿用本仓库 `chuankou/`、`laba/` 同一套：`6a400001/0002/0003-b5a3-f393-e0a9-e50e24dcca9e`，广播名 `kskbl zdjd` |
| GPIO29/41 不可用 | `esp-idf/components/soc/esp32s31/include/soc/soc_caps.h` 第 235 行 `SOC_GPIO_VALID_GPIO_MASK`，第 241 行注释 *excluding GPIO29/41 which are not bonded* |
| I2S API | `esp-idf/components/esp_driver_i2s/include/driver/i2s_std.h`（`I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` 等） |
| Flash / 芯片 | 实机 `esptool` 输出：`ESP32-S31 (revision v0.0)`、16MB flash、MAC `1c:29:04:d0:40:52` |

音色 ID 只列了官方文档示例中出现过的 `BV700_streaming`；其余音色请从火山控制台「音色列表」复制，
本仓库不臆造未经验证的音色名。
