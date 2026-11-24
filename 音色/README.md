# 音色 — ESP32-S31 云端语音合成（BLE → DeepSeek TTS → 功放）

> 模块编号 03。目录名 `音色`——这个模块存在的唯一理由就是**换音色**：
> SYN6288 只能固化一个发音人，要男女声、多语种就得走云端 TTS。

手机 BLE 连 `kskbl zdjd`，往写特征发中文，ESP32-S31 走 HTTPS 调 DeepSeek 语音合成，
拿回 PCM 直接推功放出声。**音色在网页控制台里选，Key 也在网页里填。**

本目录是 **ESP-IDF 工程根**，与仓库里 `串口/`、`喇叭/` 并列，不是仓库根目录的主工程。

---

## 1. 为什么另起一个模块

`喇叭/` 用的是 SYN6288，那颗芯片是**喂文本、吐模拟声**的封闭器件：

- 输入只有 UART 文本，音频由芯片内部 DAC 直接推功放（手册 §1.7 引脚 13 `BP0` / 15 `BN0`）
- 音色固化了**一个**，手册全篇没有任何切换发音人的命令或标记
- ESP32 碰不到音频数据，所以「软件变调换音色」这条路物理上行不通

要换音色只有走云端 TTS，而云端返回的是**数字音频流**，SYN6288 没有音频输入口。
所以这不是"改个配置"，是整条播放链路换掉——因此单独成模块，`喇叭/` 原样保留当离线兜底。

| | 喇叭（SYN6288） | 音色（本模块） |
|---|---|---|
| 出声路径 | UART 文本 → 芯片内部合成 | HTTPS 取 PCM → I2S → 功放 |
| 音色 | 固定一个 | 两个（女声 stella / 男声 tide） |
| 语速 | `[t0]`~`[t5]` 六档 | **不可调**（接口不支持） |
| 联网 | 不需要 | 必须 |
| 占用引脚 | UART1: GPIO8/9/12 | I2S: GPIO15/16/17 |

---

## 2. 链路

```
手机 BLE 写特征 (6a400002…)
   ↓ speak_feed()      累积，静默 300ms 视为一段说完
   ↓ pick_cut()        按标点切块，单块 ≤1024 字节
   ↓ ds_tts_speak()    POST https://api.deepseek.com/v1/tools/tts
   ↓ 响应体直接就是裸 PCM（边合成边返回），边收边攒样本
   ↓ audio_out_write_mono16()  按后端决定衰减 / 复制声道
   ↓ I2S0 @24kHz 16bit
功放（PDM→RC→PAM8403 / I2S→MAX98357A）
   ↓
喇叭
```

状态经 Notify 特征回传：

| 通知 | 含义 |
|---|---|
| `OK:PLAY` | 第一块开始合成 |
| `OK:NEXT` | 后续块开始合成 |
| `OK:DONE` | 全部播完 |
| `OK:STOP` | 被 `speak_stop()` 打断 |
| `ERR:NOWIFI` | Wi-Fi 没连上 |
| `ERR:NOKEY` | 当前引擎的凭据没填（DeepSeek 缺 Key / 豆包缺 AppID 或 Token） |
| `ERR:ENGINE` | 引擎是豆包，但豆包后端尚未接入 |
| `ERR:KEY` | HTTP 401/403，Key 不对 |
| `ERR:TEXT` | HTTP 400，文本为空或没过内容审核 |
| `ERR:BUSY` | HTTP 429，并发请求超限 |
| `ERR:SRV` | HTTP 503/504，服务繁忙或合成超时 |
| `ERR:NET` | 其它网络/解析错误 |

---

## 3. 接线

代码里用 `audio_out.h` 的 `AUDIO_OUT_BACKEND` 二选一，默认走 **PAM8403（PDM）**。

### 3.1 方案 A：PAM8403 等模拟功放（默认，`AUDIO_BACKEND_PDM`）

**必须先搞清楚一件事：ESP32-S31 没有 DAC。**
`components/soc/esp32s31/include/soc/soc_caps.h` 里只有 `SOC_SDM_SUPPORTED`，
没有 `SOC_DAC_SUPPORTED`。所以没法像 ESP32-S3 那样直接 DAC 输出模拟音频，
只能走 I2S 的 **PDM DAC 模式**：软件写 PCM，硬件 `PCM2PDM` 滤波器把它调制成
1bit 脉冲流从引脚出来，外部 RC 积分后才是模拟波形。

接线（只占一根信号线）：

| ESP32-S31 | 中间元件 | PAM8403（HW-104 板） |
|---|---|---|
| GPIO17 | → R1 **1kΩ** → 节点 A | |
| 节点 A | → C1 **10nF** → GND | |
| 节点 A | → C2 **0.1~1µF**（隔直）→ | **L**（左声道输入） |
| GND | — | **G**（必须共地） |
| 5V | — | **5V** |
| | | OUT_L+ / OUT_L− → 无源喇叭 4Ω 或 8Ω |

- **R1 + C1 是低通，不能省**。PDM 载波是 `128 × 24000 = 3.07MHz`，`1kΩ × 10nF`
  的截止频率约 15.9kHz，对载波衰减约 46dB。直接接功放会听到持续的高频啸叫。
- **C2 是隔直**。PDM 静音时输出是 1.65V 左右的直流（占空比 50%），
  PAM8403 输入内部有偏置，不隔直会把工作点顶偏。
- **PAM8403 是 BTL 输出**：喇叭只接 OUT+ 和 OUT− 两个端子，**任何一端都不能接 GND**，
  也不能接共地耳机/共地音源，否则烧芯片。
- **GPIO16 仍然被占用**（PDM 时钟脚），但这一脚**不需要接线**，留空即可。
- 增益固定 24dB 不可调，所以软件侧做了 `AUDIO_PDM_ATTEN_SHIFT = 3`（−18dB）的
  数字衰减，防止 3.3Vpp 直接灌进去削波。声音小改 2（−12dB），破音改 4（−24dB）。

元件只有 4 个：`1kΩ ×1`、`10nF ×1`、`0.1µF ×1`、`4Ω/8Ω 喇叭 ×1`。
10nF 就是丝印 **103**，0.1µF 就是 **104**，手边最常见的两种瓷片电容。

### 3.2 方案 B：MAX98357A 等 I2S 数字功放（`AUDIO_BACKEND_I2S`）

| ESP32-S31 | 方向 | 功放模块 | 说明 |
|---|---|---|---|
| GPIO15 | → | BCLK | 位时钟 |
| GPIO16 | → | LRC / WS | 帧时钟，有的模块标 LRCLK |
| GPIO17 | → | DIN / SD | 音频数据 |
| 3V3 | → | VIN | MAX98357A 可接 5V 得更大功率 |
| GND | → | GND | 必须共地 |

- 不需要 MCLK。MAX98357A、PCM5102、UDA1334 这类都不需要，代码里 `mclk = I2S_GPIO_UNUSED`。
- 不需要任何外围元件，没有载波残留，音质比 PDM 方案明显好，代价是要单独买模块。
- 切到这个方案：把 `audio_out.h` 里 `#define AUDIO_OUT_BACKEND AUDIO_BACKEND_PDM`
  改成 `AUDIO_BACKEND_I2S`，业务代码不用动。

### 3.3 引脚冲突核对

GPIO15/16/17 均在 `soc_caps.h` 确认有效：`SOC_GPIO_VALID_GPIO_MASK` 只排除了
**GPIO29 与 GPIO41**（注释写明 *not bonded*，未键合）。

与 `串口`（UART0: GPIO58/59）、`喇叭`（UART1: GPIO8/9/12）、USB PHY（GPIO33/34）
**完全不冲突**，三个模块的线可以同时接着。

### 3.4 关于音量

DeepSeek 的 TTS 接口不支持调音量，音量只能在本地定：

- PDM 方案：改 `AUDIO_PDM_ATTEN_SHIFT`（每加 1 减 6dB）。
- I2S 方案：MAX98357A 板上有 GAIN 焊盘/电阻，默认 9dB，可改到 12/15dB。

---

## 4. 配网与控制台

模块跑 **APSTA**：热点常开，同时尝试连路由器。

1. 手机连热点 **`kskbl-setup`**（开放的，无需密码）
2. 浏览器打开 **http://192.168.4.1/**
3. 四个页签：
   - **连接**：填 Wi-Fi 名称和密码（只支持 2.4GHz，5GHz 连不上）
   - **引擎**：先选引擎，再填对应凭据
     - `DeepSeek`：一个 **API Key**（`sk-` 开头）
     - `豆包 · 火山引擎`：**AppID** + **Access Token** + Cluster（默认 `volcano_tts`）+ voice_type
     - 两套凭据**同时保存**，切换引擎不用重填；但**豆包后端目前还没接入**，详见 9 节
   - **音色**：选 `stella`（海星·女声）或 `tide`（暗潮·男声），只对 DeepSeek 生效
   - **试听**：输入一句话点播放，直接听效果；下面还有设备状态表和恢复出厂

为什么热点常开：路由器换密码、搬家、改了 SSID 之后，STA 连不上时设备不会失联，
手机连上热点就能改配置——比"配错就成砖"友好得多。

配置全部存 NVS（namespace `lcfg`），掉电不丢。**源码和仓库里不出现任何密钥。**
控制台只回 `has_key` 布尔值，**从不把 Key 本身回传给前端**。

---

## 5. 云端接口（DeepSeek）

来源：[语音合成指南](https://api-docs.deepseek.com/zh-cn/guides/tts/)、[API 参考](https://api-docs.deepseek.com/api/tts)

```
POST https://api.deepseek.com/v1/tools/tts
Authorization: Bearer <API_KEY>        ← 空格分隔，标准写法
Content-Type: application/json
```

```json
{
  "text": "你好，我是语音合成模块",
  "voice_id": "stella",
  "format": "pcm",
  "filter_markdown": true
}
```

**响应体就是音频本身**，不是 JSON：

| format | 返回 |
|---|---|
| `pcm`（本模块用） | 裸流，24 kHz / 16 bit / 单声道 / 有符号小端，`Content-Type: audio/pcm` |
| `wav`（默认） | 带头的 WAV，`Content-Type: audio/wav` |

关键约束（都写进代码了）：

- **音频边合成边返回**，所以不用等整段合成完才出声，首包延迟很低
- 单次请求上限：**10000 计费字符**、请求体 **64 KiB**、音频 **30 分钟**
- 一个账号同时最多 **50 个在途请求**，超了返回 429
- `format=pcm` 的采样率**固定 24 kHz**，没有参数可调——所以 `AUDIO_SAMPLE_RATE` 写死 24000
- **不支持调语速，也不支持中断后续传**（官方原文）
- `filter_markdown` 默认 true，会把 `*`、`#` 这类 Markdown 符号去掉不念出来

计费（官网价目）：每 1 万字符 $0.45，限时五折 $0.225。**一个汉字算 2 个字符**，
其他字符（字母、数字、空格）算 1 个。

### 5.1 豆包（火山引擎）接口预研

来源：[V1 HTTP 非流式接口](https://www.volcengine.com/docs/6561/79823)。控制台已经能填，
后端还没接，这里先把坑记下来，免得下次重新查。

```
POST https://openspeech.bytedance.com/api/v1/tts
Authorization: Bearer; <token>        ← 分号分隔，不是空格
Content-Type: application/json
```

```json
{
  "app":   {"appid": "3145xxxxxx", "token": "任意非空", "cluster": "volcano_tts"},
  "user":  {"uid": "esp32s31"},
  "audio": {"voice_type": "BV700_streaming", "encoding": "pcm", "rate": 24000, "speed_ratio": 1.0},
  "request": {"reqid": "<每次唯一>", "text": "你好", "text_type": "plain", "operation": "query"}
}
```

- **响应是 JSON，不是音频裸流**：音频在 `data` 字段里，**base64 编码**，解码后才是 PCM
- `encoding=pcm` 可以直接要 PCM，不用引 MP3 解码器
- 文本上限 **1024 字节**（UTF-8），比 DeepSeek 的 64 KiB 小得多
- `reqid` 每次必须唯一，服务端靠它追踪
- **非流式**：全部合成完才一次性返回 —— 这就是它暂时不能用的原因，见第 9 节

---

## 6. 两个刻意的设计取舍

### 6.1 不做 JSON / base64 解析

上一版接的是火山 V1，那个接口把整段音频 base64 塞进 JSON 的 `data` 字段，
必须用状态机流式扫描 + 增量 base64 解码才塞得进 SRAM。

DeepSeek 这个接口直接返回裸 PCM，**这两个环节整个删掉了**：
读到多少字节就拼成样本推 I2S，省掉一层缓冲和一套解码器，出错面也小得多。

内存占用：读缓冲 1 KB + 样本缓冲 1 KB + 请求体 ~2.3 KB，
全程不缓存整段音频（本工程 `CONFIG_SPIRAM` 是关的，一段 20 秒语音就有 960 KB，
攒在内存里必 OOM）。

### 6.2 按标点切块，而不是按句切

音频既然是流式返回的，理论上整段丢进去最省事——中间没有任何断点。

但单次请求有上限，而且**一次失败会丢掉整段**。所以按 **1024 字节**（约 340 汉字）切块：

- 切点优先取块尾之前最后一个标点，且不能太靠前（≥ 1/3 位置）
- 找不到合适标点就硬切，但**必须退到 UTF-8 字符边界**，不会把一个汉字劈成两半
- 正常一两句话的输入只会切成一块，听感上是连续的一段

实现见 `speak.c::pick_cut()`。

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
cd /d "C:\Users\yyn\Desktop\ESP32-S31  ks\音色"
idf.py --preview set-target esp32s31
set IDF_PY_BUILD_JOBS=3
idf.py --preview bootloader
idf.py --preview build
idf.py --preview -p COM8 flash
```

- `-j` 在本机非法，并行度只能用环境变量 `IDF_PY_BUILD_JOBS`（8GB 内存建议 3）
- 烧录口：板载 CH343（VID 1A86 / PID 55D3）
- **COM8 上看不到任何日志**：console 被让位给 USB Serial/JTAG
  （`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`、`CONFIG_ESP_CONSOLE_UART_NUM=-1`）。
  要看日志请插板子的**原生 USB 口**，会出现一个 Espressif CDC 端口。

### 7.3 本机编译踩到的三个坑

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

**坑三：中文目录名编不过（重要）。** 目录改名成 `音色` 之后：

- PowerShell 5.1 调 `idf.py` 时把路径按 ANSI 转码，CMake 收到的是 `闊宠壊`
- 换成 cmd 直调也过不了：`riscv32-esp-elf-gcc` 报
  `cannot read spec file '...\x0a\build\...\picolibc.specs'`，中文被转成了换行符
- 目录联接（junction）会被 CMake 解析回真实中文路径；`dir /x` 也没有生成 8.3 短名

结论：**ESP-IDF 的工具链链路处理不了非 ASCII 路径**，`config` 能过、`build` 过不了。
要编译就把工程放到纯 ASCII 路径（例如 `C:\espwork\yinse`），或在仓库里保留英文目录名。

### 7.4 编译实测（2026-11-24，DeepSeek 版）

因为坑三，这次编译是在纯 ASCII 路径 `C:\espwork\yinse` 下做的，源码与 `音色/` 逐字节一致（`diff -r` 无差异）。

| 项 | 结果 |
|---|---|
| 编译结果 | `Project build complete`，0 error / 0 warning |
| app 二进制 | `0x1a2990` = 1,714,576 字节（1.64 MB） |
| app 分区 | `0x400000`（4 MB），剩余 `0x25d670`（59%） |
| bootloader | `0x4cc0`（19.6 KB），剩余 20% |
| 源码 | 7 个 .c 全部编过，含 `ds_tts.c`（新）、`cfg_store.c`、`speak.c`、`wifi_prov.c` |
| 分区表 | `factory` @ `0x10000`，size `0x400000`（4 MB），已用 `od` 核对二进制 |

烧录偏移与旧版一致：bootloader `0x2000`、partition-table `0x8000`、app `0x10000`。

---

## 8. 验收

1. 手机连 `kskbl-setup`，开 http://192.168.4.1/
2. 「连接」页填 Wi-Fi，保存 → 徽章变绿，状态表里出现 IP
3. 「引擎」页选 `DeepSeek`，填 API Key，保存 → 徽章变「DeepSeek 就绪」
4. 「试听」页点播放 → 喇叭出声
5. 手机 BLE 助手连 `kskbl zdjd`，订阅 `6a400003…` 的 CCCD
6. 往 `6a400002…` 写中文，例如 `你好，我是语音合成模块`
7. 预期：喇叭播报，Notify 依次收到 `OK:PLAY` → `OK:DONE`

---

## 9. 已知限制

- **只有两个音色**（stella 女声 / tide 男声），这是 DeepSeek 接口的全部
- **豆包引擎只做了配置项，后端还没接入**。原因：火山 V1 是**非流式**接口，
  整段音频以 base64 塞在 JSON 里一次返回，24kHz/16bit 单声道约 48KB/秒，
  10 秒语音就是 640KB base64，ESP32 的堆扛不住。要接得先定策略——
  要么把每块文本压到一两句（非流式 + 分块），要么改用 v3 的流式接口。
  现在切过去只会回 `ERR:ENGINE`
- **不能调语速和音量**，官方明确写「不支持调节语速」。要调音量只能在软件里乘系数，
  或改功放增益焊盘
- **不支持断点续传**：音频已经开始返回后如果合成失败，响应会中途断开，
  这一块就缺一截（代码会把提前结束当成失败报出来）
- **`stella` 是女声，中文读法偏书面**，实测之前建议先用「试听」页听一遍
- **未实机验证**：编译已通过，但 API Key、I2S 功放出声、配网流程都还需要接上实物跑一遍

---

## 10. 出处与说明

| 内容 | 来源 |
|---|---|
| TTS 接口、参数、音色、限制 | [DeepSeek 语音合成指南](https://api-docs.deepseek.com/zh-cn/guides/tts/) / [API 参考](https://api-docs.deepseek.com/api/tts) |
| 计费与五折 | 同上文档的 Billing 章节 |
| BLE 服务与 UUID | 沿用本仓库 `串口/`、`喇叭/` 同一套：`6a400001/0002/0003-b5a3-f393-e0a9-e50e24dcca9e`，广播名 `kskbl zdjd` |
| GPIO29/41 不可用 | `esp-idf/components/soc/esp32s31/include/soc/soc_caps.h` 第 235 行 `SOC_GPIO_VALID_GPIO_MASK`，第 241 行注释 *excluding GPIO29/41 which are not bonded* |
| I2S API | `esp-idf/components/esp_driver_i2s/include/driver/i2s_std.h`（`I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` 等） |
| Flash / 芯片 | 实机 `esptool` 输出：`ESP32-S31 (revision v0.0)`、16MB flash、MAC `1c:29:04:d0:40:52` |

音色只列官方文档给出的 `stella` 和 `tide`，本仓库不臆造未经验证的音色名。
