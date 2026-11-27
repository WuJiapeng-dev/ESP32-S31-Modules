# 小智ai接入 — ESP32-S31 语音对话（INMP441 + MAX98357 + DeepSeek）

把"说话 → 识别 → 大模型回答 → 喇叭念出来"整条链跑在一块 ESP32-S31 上。
云端只用一个 DeepSeek API Key 就够：**ASR（识别）、LLM（回答）、TTS（合成）都是 DeepSeek 官方接口**。

---

## 1. 能力边界（先说清楚，避免误解）

| 环节 | 用什么 | 依据 |
|---|---|---|
| 语音识别 ASR | DeepSeek `POST /v1/tools/asr` | https://api-docs.deepseek.com/zh-cn/guides/asr/ |
| 大模型 LLM | DeepSeek `POST /chat/completions`，模型 `deepseek-flash` | https://api-docs.deepseek.com/zh-cn/ |
| 语音合成 TTS | DeepSeek `POST /v1/tools/tts`，`format=pcm`（24kHz/16bit/单声道裸流） | https://api-docs.deepseek.com/zh-cn/guides/tts/ |

查证结论（2026-11-27 核对官方文档）：

- DeepSeek **确实提供** 语音识别与语音合成接口，不是只有文本 chat。
- ASR 一次请求发整段音频（`multipart/form-data` 的 `file` 字段，16bit PCM WAV 或 Ogg Opus，
  采样率 8000~48000，单次最长 120 秒、最大 8MiB），返回整段文本，**不支持流式**。
- TTS 支持整段 HTTP 与 WebSocket 流式两种；HTTP 版响应体就是音频裸流，
  `format=pcm` 时固定 24kHz/16bit/单声道，正好可以直接灌 I2S 给 MAX98357，省掉 MP3 解码。
- TTS 只有两个音色：`stella`（女声，默认）、`tide`（男声）；不支持调语速/音量。

所以本工程**不需要** Opus 编解码、不需要自建服务器、不需要第三方 ASR/TTS 账号。

---

## 2. 接线（用户指定，逐字采用）

### 麦克风 INMP441（I2S 数字麦）

| INMP441 | ESP32-S31 | 说明 |
|---|---|---|
| SCK / BCLK | GPIO19 | I2S 位时钟 |
| WS / LRCLK | GPIO46 | 帧同步 |
| SD / DOUT | GPIO13 | 数据（麦克风 → 芯片） |
| L/R | VDD (3V3) | 接高电平 = 右声道输出，代码里 `slot_mask = RIGHT` |
| VDD | 3V3 | |
| GND | GND | |

### 功放 MAX98357A（I2S 数字功放）

| MAX98357 | ESP32-S31 | 说明 |
|---|---|---|
| BCLK | GPIO57 | I2S 位时钟 |
| LRC / WS | GPIO56 | 帧同步 |
| DIN | GPIO38 | 数据（芯片 → 功放） |
| SD / GAIN | GPIO37 | 芯片手册 SD_MODE：低=shutdown，高=使能。代码在播放前后翻转 |
| VIN | 5V 或 3V3 | 按模块标称 |
| GND | GND | 必须共地 |
| OUT+ / OUT- | 喇叭 | 无源喇叭 |

两组 I2S 是**两个独立通道**：`I2S_NUM_0` 做 RX（麦克风），另一个通道做 TX（功放），互不干扰。

> 引脚合法性：ESP32-S31 共 62 个 GPIO（`SOC_GPIO_PIN_COUNT=62`），
> 有效掩码排除 GPIO29 与 GPIO41。57/56/38/37/19/46/13 全部合法。

---

## 3. 编译与烧录

### 3.1 环境

- ESP-IDF **v6.1.0**（`D:\esp\v6.1\esp-idf`）
- ESP32-S31 是 preview 目标，**所有 `idf.py` 命令必须带 `--preview`**
- 必须在已激活 IDF 环境的终端里执行

### 3.2 中文路径的处理（重要）

工程目录含中文与空格（`ESP32-S31  ks\小智ai接入`），CMake/Ninja 在非 ASCII 路径下会出错。
已用 `subst` 把它映射成一个盘符，编译时走 ASCII 路径：

```bat
subst X: "C:\Users\yyn\Desktop\ESP32-S31  ks\小智ai接入"
```

之后在 `X:\` 下编译即可。工程里附带的 `build.bat` 就是这套流程（可复制到工程根目录使用）：

```bat
@echo off
call D:\esp\v6.1\esp-idf\export.bat
cd /d X:\
if not exist build idf.py --preview set-target esp32s31
idf.py --preview build
```

烧录：

```bat
cd /d X:\
idf.py --preview -p COM8 flash monitor
```

（COM 口按设备管理器里的 CH343 实际编号改。）

### 3.3 如果不用 subst

把整个 `小智ai接入` 目录复制到纯英文路径（如 `C:\espwork\xiaozhi_ai`）再编译，效果相同。

---

## 4. 配网与 API Key 配置

密钥**不写在源码里**，全部运行时写入 NVS：

1. 上电后板子开一个开放热点 **`kskbl-setup`**（APSTA 常驻，连上路由器后热点依然在）。
2. 手机连这个热点 → 浏览器打开 **http://192.168.4.1/**。
3. 填三样东西后提交：
   - **Wi-Fi 名称 / 密码**（2.4GHz，S31 不支持 5G）
   - **DeepSeek API Key**（`sk-...`，在 https://platform.deepseek.com/api_keys 申请）
   - **音色**：`stella`（女声，默认）或 `tide`（男声）
4. 提交后写入 NVS 并重启，串口会打印连网过程。

页面里还有「试听」按钮，可以直接验证 Key 与喇叭通不通，不用说话。
页面上那些豆包/火山引擎字段本工程不使用，留空即可。

改配置：任何时候重新连 `kskbl-setup` 打开同一个网址即可，不需要重新烧固件。

---

## 5. 使用

上电 → 串口出现 `就绪：直接对着板子说话即可` → 对着 INMP441 说一句中文 → 松口约 0.7 秒后自动开始处理。

串口会依次打印：

```
【识别】今天天气怎么样
【AI】今天北京晴，气温 3 到 12 度，出门记得穿外套。
播报完成：xxxxxx 字节 PCM，耗时 xxx ms，HTTP 200
```

不需要按键、不需要唤醒词：能量端点检测到有人说话就开录。

**播报期间不录音**。原因：MAX98357 出来的声音会被 INMP441 直接收进去形成自激，
本工程不做回声消除，用"播放时暂停采集"这个最省事也最可靠的办法。
所以**不支持"说话打断播报"**——这是有意的取舍，不是 bug。

---

## 6. 参数总表

| 参数 | 值 | 位置 |
|---|---|---|
| 麦克风采样率 | 16000 Hz / 16bit / 单声道（I2S 32bit 槽取高位） | `mic_i2s.h` |
| 采集帧长 | 30 ms（480 样本） | `dialog.c` |
| VAD 噪声底跟踪 | 0.95/0.05 一阶平滑，下限 30 | `dialog.c` |
| 触发阈值 | `max(3×噪声底, 300)`，连续 3 帧过阈 | `dialog.c` |
| 语音前预滚 | 200 ms | `app_config.h` |
| 判说完静音 | 700 ms | `app_config.h` |
| 单句最长 | 8 秒 | `app_config.h` |
| 最短有效语音 | 400 ms | `app_config.h` |
| 录音缓冲 | 256KB（PSRAM），失败降级到内部 RAM 3 秒 | `dialog.c` |
| TTS 输出 | 24000 Hz / 16bit / 单声道 PCM | `audio_out.h` |
| 对话历史 | 最近 3 轮，纯内存 | `app_config.h` |
| LLM 单次上限 | 320 tokens | `app_config.h` |

---

## 7. 排查表

| 现象 | 先查什么 |
|---|---|
| 串口一个中文字都没有 | 控制台在 **USB Serial/JTAG**（不是 UART0），要用板子的 USB 口；波特率 115200 |
| 说话没反应 | 串口看有没有「检测到说话」。没有 → 查 INMP441 三根线 19/46/13、L/R 是否接 3V3、3.3V 供电 |
| 一直「检测到说话」 | 环境太吵或麦克风增益过高；噪声底会自适应，安静后应恢复 |
| 识别失败 HTTP 400 | 音频格式问题（本工程固定 16bit WAV 单声道，正常不该出现） |
| 识别失败 HTTP 401 | API Key 错或过期，重新在网页填 |
| 识别失败 HTTP 402 | DeepSeek 账户余额不足 |
| LLM 失败 HTTP 402/429 | 同上；429 是并发/限流，等几秒再说 |
| 喇叭不响 | 按顺序查：① GPIO37 是否拉高（串口有「MAX98357 使能」）② 57/56/38 三根线 ③ 喇叭接 OUT+/OUT- ④ VIN 供电 |
| 播报有杂音/断续 | 供电不足（MAX98357 峰值电流大，建议 5V + 大电容）；或 Wi-Fi 与 I2S 抢 DMA |
| 连不上 Wi-Fi | 只支持 2.4GHz；SSID/密码里不要有全角字符 |

---

## 8. 工程结构

```
小智ai接入/
├── CMakeLists.txt          顶层工程
├── sdkconfig.defaults      CPU/PSRAM/Wi-Fi/TLS/分区
├── partitions.csv          app 4MB
├── README.md
└── main/
    ├── main.c              初始化顺序与启动日志
    ├── app_config.h        引脚以外的全局参数（接口 URL、模型名、VAD 参数）
    ├── mic_i2s.c/.h        INMP441 采集（19/46/13）
    ├── audio_out.c/.h      MAX98357 播放（57/56/38/37，24kHz）
    ├── dialog.c/.h         一轮对话状态机 + 能量 VAD + 录音
    ├── asr_client.c/.h     DeepSeek ASR（multipart 上传 WAV）
    ├── llm_client.c/.h     DeepSeek chat（带 3 轮历史）
    ├── ds_tts.c/.h         DeepSeek TTS（边收边播 PCM）
    ├── cfg_store.c/.h      NVS 配置（Wi-Fi / Key / 音色）
    └── wifi_prov.c/.h      SoftAP 配网 + 网页控制台
```

`audio_out.c/.h`、`ds_tts.c/.h`、`cfg_store.c/.h`、`wifi_prov.c/.h` 来自本仓库
`音色/` 模块（那套代码已在 S31 上编译验证过），本工程只改了功放后端与引脚。
`mic_i2s.c/.h` 来自 `喇叭/复述/` 的 INMP441 采集实现。

---

## 9. 与 xiaozhi-esp32 参考工程的关系

`xiaozhi_audio_ai/` 是 xiaozhi-esp32 的残缺提取（91 个文件，缺 display/led/network/assets/notify
等目录，从未编译过），它的音频链路依赖 AFE 引擎 + Opus + 自建 WebSocket 服务端。

本工程**沿用了它的三个关键判断**（见 `源码剖析报告.md` 第 5 章参数总表）：

1. 上行音频参数：16kHz / 单声道 / 16bit；
2. 端点检测、预滚、静音判停的思路；
3. "播报期间不录音、靠状态机切换"的线程纪律。

但**没有照搬**它的传输层：xiaozhi 要求服务端先下发 `tts start` 再推 Opus 流，
而本工程用 DeepSeek 的 HTTP 接口，一段文本一次请求换回裸 PCM，链路更短、故障点更少。
用 Opus 的唯一收益是省流量，对家庭 Wi-Fi 场景不值得多编进一个编解码器。

---

## 10. 出处

- 接线：用户指定（INMP441 19/46/13，MAX98357 57/56/38/37）。
- MAX98357A 引脚语义：芯片手册 `MAX98357A/MAX98357B` Pin Description（SD_MODE 低=shutdown）。
- DeepSeek ASR / LLM / TTS 的 URL、请求体、响应格式：官方文档三页，见第 1 节链接。
- 音频参数与状态机思路：`xiaozhi_audio_ai/源码剖析报告.md` 第 5 章。
- 配网网页、NVS 存储、I2S 输出：本仓库 `音色/` 模块（已编译验证）。
