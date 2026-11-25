# 麦克风（板载）— ESP32-S31 板载麦克风识别 → BLE 送手机显示

对着**板载麦克风**说话 → ESP32 离线识别成文字 → 经 BLE Notify 推到手机显示。
**不接喇叭**：没有 SYN6288、没有 UART1、没有 TTS 与 GB2312 转码。
**不需要外接任何麦克风模块。**

## 0. 硬件：板载麦克风走 ES8389 codec（已实测跑通）

板型 **慧勤智远 Waiken-Smart ESP32-S31 CB V1.1**（原理图 Rev 2026-09-17，
用户手册 V1.0）。音频链路是：

```
MIC1 / MIC2 (LMA3729T381-OAS02 模拟 MEMS) ──► ES8389 双 ADC
                                              │
                                    I2S_ASDOUT (GPIO6)
                                              ▼
                                            ESP32-S31
```

**不用接任何外接麦克风，也不用焊线。** 引脚全部取自厂商原理图，不是猜的：

| 信号 | GPIO | 说明 |
|---|---|---|
| I2C SDA | **0** | 与触摸 GT1151Q(0x14)、摄像头共用一条总线，板上有 4.7k 上拉 |
| I2C SCL | **1** | 注意 SDA=0 / SCL=1，两个不能对调 |
| I2S BCLK (SCLK) | **3** | ESP32 输出，16kHz × 16bit × 2 声道 = **512kHz** |
| I2S WS (LRCK) | **4** | 帧时钟 |
| I2S ASDOUT | **6** | codec → ESP32，录音方向（这一根没接对就永远没数据） |
| PA_CTRL | **7** | 两颗 NS4150B 功放使能，高电平开；本工程录音时**拉低** |
| （I2S MCLK） | 2 | **本方案不驱动**，codec 从 BCLK 派生内部时钟，见 §0.2 |

ES8389 7 位地址 **0x10**（8 位写法 0x20），AD0 经 100kΩ 下拉。

### 0.1 为什么之前折腾 ES8311 / PDM 全都失败

不是芯片坏了，是**板子型号认错了**：早期按乐鑫 `ESP32-S31-Function-CoreBoard-1`
的资料去配（ES8311 + I2C 50/51 + I2S 52/53/55/54），而这块是慧勤的 CB V1.1：
codec 是 **ES8389**、I2C 在 **0/1**、I2S 在 **2/3/4/6**。所以：

- 扫 I2C 50/51 得到 0 个设备 → 因为那条总线上本来就没有音频器件；
- 换成 PDM 后端能初始化但读不到声音 → 因为板载麦是模拟麦，不是 PDM 数字麦。

修正后在实物上的日志（2026-11-25，COM8）：

```
I (343) mic: I2C 探测到 ES8389（SDA=GPIO0 SCL=GPIO1，7bit 0x10）
I (373) mic: I2S 已启动: BCLK=GPIO3 WS=GPIO4 DIN=GPIO6，BCLK=512kHz
I (393) I2S_IF: STD: RX, data_bit: 16, slot_bit: 16, slot_mode: STEREO
I (403) Adev_Codec: Open codec device OK
I (533) mic: 电平自检: 左 RMS=70 峰值=346 | 右 RMS=57 峰值=258（全 0 = 没有数据）
```

最后那行是**开完 codec 立刻量 125ms 的电平**：两路都有非零数据，说明
麦克风真的在送样本；若两路都是 0，日志会直接报错并提示查 ASDOUT。

### 0.2 关键约束：本方案让 codec 从 BCLK 派生时钟（use_mclk = false）

`esp_codec_dev` 的 ES8389 驱动里，分频寄存器只在 **不用 MCLK** 时才写：

```c
if (!codec->use_mclk) es8389_config_sample(codec, fs->sample_rate, fs->bits_per_sample);
```

一旦 `use_mclk = true`，它**根本不写** 0x04~0x0A、0x21、0x22 这些分频寄存器，
16kHz 就落不到系数表里（表现为能打开但采样率不对）。厂商例程 `19_music`
同样不送 MCLK，所以这里跟着走 BCLK 派生：

- 16bit × 2 声道 → BCLK/LRCK 比值 **32**
- 系数表里正好有 `{Ratio=32, MCLK=512000, LRCK=16000}`，即 BCLK = **512kHz**
- 采样率 16kHz 不变，MFCC/DTW 一行都没改

### 0.3 增益与声道

- 两路麦分别落在**左/右声道**，识别只用一路，默认取**左声道**
  （`MIC_CH_SEL`：0=左 1=右 2=两路平均）。
- PGA 默认 **30dB**（`MIC_PGA_DB`）。安静房间实测噪声底 RMS≈70；
  若说话时 RMS 不到约 500，改 33.5 或 36；出现削波（峰值贴 32767）就降到 27.5。

### 0.4 另一路后端：外接 INMP441

`mic_i2s.h` 改一行 `#define MIC_BACKEND MIC_BACKEND_INMP441` 即可，
接法 SCK=GPIO19、WS=GPIO46、SD=GPIO13、L/R 接 3V3、VDD 接 3V3，代码已保留。

与同级的 `../喇叭/手机接收`（外接 INMP441 版）是同一个识别内核的两套采集实现。

共用同一套 BLE 身份：广播名 **`kskbl zdjd`**，Service `6a400001-…`、
Write `6a400002-…`、Notify `6a400003-…`（CCCD `00002902-…` 由协议栈自动生成）。

### 0.5 资料来源

- 慧勤智远 ESP32-S31 CB V1.1 原理图（4 页）、用户手册 V1.0 引脚分配表
- 同板资料整理：`docs/boards/wks_s31_cb.md`（zhing2006/esp-gbx 仓库，
  记录了 ES8389 的 I2C 0x10、I2S 2/3/4/5/6、PA_CTRL 7、MIC1/MIC2 型号）
- `esp_codec_dev` 组件内的 `device/es8389/es8389.c`（系数表、`use_mclk` 分支）

---

## 1. 离线 ASR 可行性（先说结论）

**通用中文大词表 ASR 在 ESP32-S31 上跑不动；本工程用"自学习模板匹配"，
能认出你教过的句子，认不出没教过的任意中文。**

| 方案 | 实际情况 | 结论 |
|---|---|---|
| **esp-sr**（乐鑫 WakeNet / MultiNet） | IDF v6.1 内置 147 个组件里**没有** esp-sr；它由组件管理器单独安装，并按芯片提供预编译静态库（esp32/s2/s3/c3/c6/p4…），**不含 `esp32s31` 这个 preview 目标** | 不可用（不是配置问题，是没有二进制） |
| **whisper.cpp tiny** | 39M 参数，int8 量化也要 ~40MB 模型 + 上百 MB 运行内存 | 差 1~2 个数量级 |
| **vosk small-cn** | 中文小模型 42MB | 同上 |
| **云端 ASR** | 本轮要求离线 | 见 §1.2 方案 A |

### 1.1 实际方案

```
麦克风 16kHz → 每 10ms 一帧 → MFCC 13 维 → 端点检测
   → 一句话特征序列（≤2s / 200 帧）→ 与模板逐条 DTW → 距离 < 阈值 ⇒ 命中
```

- **教**：手机发 `EN:你好` → 收到 `OK:ENROLLING 你好` → 立刻说"你好" → `OK:ENROLLED 你好`
- **认**：发 `ASR` → 说"你好" → 收到 `ASR:你好` + `SCORE:6.3`
- 最多 8 条、单句最长 2 秒；说话人自己教自己用效果最好
- 没模板时明确回 `ASR:? NOTMPL`，**不编造结果**

### 1.2 想要"任意中文"（方案 A，未实现）

录到一句 → HTTPS 上传 PCM → 云端返回文本 → Notify。
接入点是 `asr.c` 的 `on_utterance_done()` 未命中分支，改造量比现在还小（不用带模型）。
难点从"算力"变成"流量与时延"。

---

## 2. 接线：不用接

板载麦克风已经在板上，信号走 ES8389 → 模组内部走线。**一根杜邦线都不用接**：

- 不接 INMP441、不接 SYN6288、不接功放
- 只有一根 USB-C（COM 口，CH343P）既供电、烧录、又出串口日志
- 喇叭可以接在 `SPKL` / `SPKR` 两个 2 针插座上，但本工程把 `PA_CTRL(GPIO7)`
  拉低，功放不工作，接了也不响，属预期

只有换到 `MIC_BACKEND_INMP441` 时才需要接线，见 §0.4。

---

## 3. 手机操作

| 步骤 | 发（Write 6a400002） | 收（Notify 6a400003） |
|---|---|---|
| 1 | `PING` | `PONG` |
| 2 | `VER` | `VER:phone_recv/1.0` |
| 3 | `EN:你好` | `OK:ENROLLING 你好` → 立刻说"你好" → `OK:ENROLLED 你好` |
| 4 | `LIST` | `LIST:1` / `T1:你好` |
| 5 | `ASR` | `OK:ASRON 1` |
| 6 | 说"你好" | **`ASR:你好`** + `SCORE:6.3` |
| 7 | `ASR:OFF` | `OK:ASROFF` |

其它命令：`DEL:1` / `DEL:你好`、`CLEAR`、`TH:25`（阈值，默认 18.0，越小越严）、`STATE`。

> 先订阅 Notify 再发命令；每条命令间隔 0.5 秒以上（驱动把连续写入拼成一条，静默 250ms 结束）。

---

## 4. 实现要点

- **I2S**：标准 Philips、16kHz、32bit 槽（INMP441 输出 24bit 左对齐，右移 16 位取高 16bit）、
  `slot_mask = RIGHT`、DMA 6×240 帧
- **端点检测**：噪声底 EMA 慢跟踪，阈值 `max(floor×3, 180)`；连续 3 帧过阈值开始，
  回捞 300ms 预滚；连续 25 帧静音结束
- **特征**：25ms/10ms、Hamming、预加重 0.97、512 点 FFT（自实现）、26 带 Mel、13 维（丢 c0）、CMN
- **匹配**：DTW + Sakoe-Chiba 带宽约束
- **内存**：模板 8×200×13×2B ≈ 42KB，语句 5.2KB，预滚 0.8KB，**音频 PCM 不缓存**（流式计算）

---

## 5. 编译与烧录

源码在中文路径，**编译必须用纯 ASCII 副本**：

```bat
:: 把本目录源码同步到 C:\espwork\phone_recv，然后
cd /d C:\espwork\phone_recv
build.bat set-target esp32s31
build.bat
flash.bat            :: 自动找 CH343 串口烧录，不重编译
```

已知编译坑（都写进 build.bat / sdkconfig.defaults 了）：

1. `partitions.csv` 必须在 sdkconfig 里显式启用（`CONFIG_PARTITION_TABLE_CUSTOM=y`），
   否则用内置 single-app，app 只有 1MB
2. `IDF_PY_BUILD_JOBS` 不要超过 2：本机 8GB + 安全软件常驻，并行 3 会把 gcc 挂死
3. 不要全局开 `CONFIG_COMPILER_OPTIMIZATION_PERF`（mbedtls 编译吃 1GB+ 内存）；
   只在本模块 `CMakeLists.txt` 里加 `-O2`
4. 控制台在 UART0（GPIO58/59），**烧录线就是日志线**，不用另插 USB

---

## 6. 验收

1. 上电，串口（CH343，115200）看到 `离线识别就绪：模板 0/8，阈值 18.0`
2. 手机连 `kskbl zdjd`，订阅 Notify，发 `PING` → `PONG`
3. `EN:你好` → 说"你好" → `OK:ENROLLED 你好`
4. `ASR` → 说"你好" → **手机显示 `ASR:你好`**（这就是本轮验收标准）
5. 说一句没教过的 → `ASR:? d=… th=18.0`，不出结果

---

## 7. 不工作时按这个顺序查

先看串口（CH343P，115200）—— 本工程的日志走 UART0，烧录线就是日志线。

| # | 看什么 | 判断 |
|---|---|---|
| 1 | `I2C 探测到 ES8389 … 0x10` | 没这行 → SDA=0 / SCL=1 接反，或 AUDIO_3V3 没起来 |
| 2 | `Open codec device OK` | 只到 `ES8389: Open fail` → 看紧随其后的 I2C 总线扫描结果 |
| 3 | `电平自检: 左 RMS=… 峰值=…` | **两路全 0** → ASDOUT(GPIO6) 没接对，或 codec 没出数据 |
| 4 | RMS 只有几十、说话也不涨 | PGA 太小 → 把 `MIC_PGA_DB` 改 33.5 / 36，重编译 |
| 5 | 有数据但永不出现 `检测到说话` | VAD 阈值 = 噪声底 × 3；噪声底高就说话大声点或提高增益 |
| 6 | 出现 `检测到说话` 但 `ASR:? d=…` | 距离超阈值：`TH:25` 放宽；认错话就调回小值 |
| 7 | `ASR:? NOTMPL` | 没教过，先 `EN:标签` |
| 8 | `PING` 没回 `PONG` | 连错设备 / 没订阅 `6a400003` 的 CCCD |
| 9 | 别指望喇叭响 | 本工程不初始化 UART1，PA_CTRL 拉低，属设计如此 |
