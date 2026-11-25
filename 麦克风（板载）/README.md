# 麦克风（板载）— ESP32-S31 板载麦克风识别 → BLE 送手机显示

对着**板载麦克风**说话 → ESP32 离线识别成文字 → 经 BLE Notify 推到手机显示。
**不接喇叭**：没有 SYN6288、没有 UART1、没有 TTS 与 GB2312 转码。
**不需要外接任何麦克风模块。**

## 0. 当前状态（先看这段）

本机板型是 **ESP32-S31 CB V1.1（慧勤智远）**，第三方 S31 开发板，**不是**乐鑫的
`ESP32-S31-Function-CoreBoard-1`。两者的音频方案不同，实测结论如下：

| 检测项 | 实测结果 |
|---|---|
| I2C GPIO50/51 全总线扫描（0x08–0x77） | **0 个设备应答** |
| MCLK（GPIO52）已启动后再配 ES8311 | 仍然 `I2C_If: Fail to write to dev 30`，`ES8311: Open fail` |

**结论：这块板没有 ES8311 类 codec**，板载麦克风不是走 codec 的模拟麦路线，
最可能是**PDM 数字麦**（只需 CLK + DATA 两根线，不占 I2C）。

因此采集层做成三种后端，编译期用 `mic_i2s.h` 里的一行宏切换：

| 宏 | 后端 | 引脚 |
|---|---|---|
| `MIC_BACKEND_PDM`（**当前默认**） | PDM 数字麦 | `MIC_PDM_CLK_GPIO` / `MIC_PDM_DATA_GPIO`，**需按实际板子填** |
| `MIC_BACKEND_ES8311` | 板载 ES8311 + 模拟麦 | I2C 50/51，I2S MCLK52 BCLK53 WS55 DIN54 |
| `MIC_BACKEND_INMP441` | 外接 INMP441 | SCK19 WS46 SD13，L/R 接 3V3 |

> ⚠️ **PDM 那两个引脚目前是占位值（46/47），必须换成这块板的真实引脚**，
> 否则读到的只会是静音或噪声。确定方法见 §0.2。

## 0.1 板载 PDM 麦克风的引脚怎么定

PDM 麦在板子上只有两根信号线：**CLK（ESP32 输出）+ DATA（麦输出）**。
确定引脚有三个途径，按可靠性排序：

1. **看原理图**（最准）：找麦克风元件旁的 `PDM_CLK` / `PDM_DATA` 网络标号
2. **看实物走线**：麦克风元件只有两根细线连到模组，对照排针丝印反推
3. **看丝印**：有些板会把 `MIC_CLK` / `MIC_DAT` 直接印在焊盘旁

改好 `mic_i2s.h` 里这两个宏，重新编译烧录，启动日志出现

```
I (331) mic: PDM 麦就绪: CLK=GPIOxx DATA=GPIOyy @ 16000Hz 16bit 单声道
```

之后对着板子说话，串口应打出 `检测到说话，开始记录（噪声底 xx，阈值 xx）`——
**这一行出现才说明麦克风真的在送数据**。

## 0.2 参考：如果换成乐鑫 Function-CoreBoard-1

那块板是 ES8311 方案（模拟麦 → ES8311 → I2S），引脚如下：

| 信号 | GPIO | 说明 |
|---|---|---|
| I2C SCL | 50 | ES8311 寄存器配置 |
| I2C SDA | **51** | 注意 SDA/SCL 不要接反 |
| I2S MCLK | 52 | 4.096 MHz = 16 kHz × 256 |
| I2S BCLK | 53 | 位时钟 |
| I2S WS | 55 | 帧时钟 |
| I2S DIN | 54 | ES8311 ASDOUT → ESP32（录音方向） |

**GPIO50–57、60、61 是板载外设专用，不要复用。**

### 0.2.1 最容易踩的坑：MCLK 必须先跑起来，ES8311 才会应答 I2C

> **MCLK（GPIO52）没有时钟输出时，ES8311 根本不在 I2C 上回应。
> 此时去 probe 0x18 一定失败，表现为"总线上一个设备都没有"，
> 很容易误判成"这块板没焊 codec"。**

`es8311_codec_new()` 内部就会读写寄存器，所以初始化顺序必须是：

```
建 I2C 总线 → 建 I2S 通道 → enable I2S（MCLK 开始输出）→ 等 20ms
   → es8311_codec_new() → esp_codec_dev_open()
```

本工程代码已按此顺序实现，诊断扫描只在 open 失败后才执行。

采样率定 16 kHz 的依据：ES8311 时钟分频表（`es8311.c` 的 `coeff_div`）里有
`{MCLK=4096000, rate=16000}` 这一行，4.096 MHz 正好是 16 kHz × 256，
因此不需要重采样，MFCC/DTW 这一层与 16 kHz 版本完全一致。

ES8311 驱动直接复用官方 `esp_codec_dev` 组件（随仓库放在 `components/`），
**不手写寄存器表**：少写一个寄存器就是安静地没声音、没有任何报错。

与同级的 `../喇叭/手机接收`（外接 INMP441 版）是同一个识别内核的两套采集实现，
`mic_i2s.h` 里一行宏 `MIC_BACKEND` 即可切换，其余代码完全相同。

共用同一套 BLE 身份：广播名 **`kskbl zdjd`**，Service `6a400001-…`、
Write `6a400002-…`、Notify `6a400003-…`（CCCD `00002902-…` 由协议栈自动生成）。

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

## 2. 接线（只需 6 根）

| INMP441 | → | ESP32-S31 | 说明 |
|---|---|---|---|
| **SCK**（BCLK） | → | **GPIO19** | 位时钟 |
| **WS**（LRCL） | → | **GPIO46** | 帧时钟 |
| **SD**（DOUT） | ← | **GPIO13** | 数据 |
| **L/R** | → | **3V3** | 接 VDD = 右声道（代码 `slot_mask = RIGHT`） |
| **VDD** | → | **3V3** | 只能 3.3V，上限 3.6V |
| **GND** | → | **GND** | 必须共地 |

**不要再接 SYN6288**：GPIO8/9/12 在本工程里完全不初始化。

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

| # | 检查 | 判断 |
|---|---|---|
| 1 | SCK/WS/SD 三根线 | 串口正常但日志里永不出现"检测到说话" |
| 2 | **L/R 是否 3.3V** | 接 GND 会选左声道，代码读右槽 → 全静音（最常见） |
| 3 | 采样率/位宽 | 串口 `I2S RX 就绪 … 16000Hz 32bit槽/取右声道` |
| 4 | 识别是否真跑 | 日志有 `语句结束: N 帧` 才说明 VAD 触发、特征在算 |
| 5 | BLE 连接 | `PING` 不回 → 连错设备 / 没订阅 Notify |
| 6 | 是否误接喇叭 | 本工程不初始化 UART1，接了也不会响，属预期 |
