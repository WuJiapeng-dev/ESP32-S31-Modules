# 喇叭 — ESP32-S31 + SYN6288 中文语音合成（BLE → TTS）

> **本目录只含一个 ESP-IDF 工程**（SYN6288 离线合成）。原先并排放在这里的两个麦克风版本
> 已迁到仓库根目录 `麦克风/` 下，按「功能 + 开发环境」分目录：
> `麦克风/Speaker-Repeat-VSCode/`（INMP441 识别 → Notify + 喇叭念出识别结果）、
> `麦克风/Phone-Receive-VSCode/`（INMP441 识别 → 只发 BLE，不接喇叭）。
> 三者互相独立，各有 `CMakeLists.txt` / `sdkconfig.defaults` / `main/`，可单独编译烧录，
> 同一块板子同一时间只能跑一个。
>
> 下面这一节讲的是**本层**这个 SYN6288 合成工程。

手机 BLE 连上 `kskbl zdjd`，往写特征发任意 UTF-8 文本，ESP32-S31 转成 GB2312 后按
SYN6288 帧协议经 UART1 下发，模块直接播报。播报状态经 Notify 特征回传手机。

本目录是 **ESP-IDF 工程根**，与仓库里 `串口/` 模块并列，不是仓库根目录的主工程。

---

## 1. 目录结构

```
喇叭/
├── CMakeLists.txt              顶层工程（project: laba_s31_tts）
├── sdkconfig.defaults          console 改 USB Serial/JTAG、NimBLE 外设+广播者、16MB Flash
├── .vscode/settings.json       IDF_TARGET=esp32s31
├── README.md                   本文件
├── flash.bat / flash.ps1       一键烧录（自动激活 IDF + 自动识别 CH343）
├── flash_verify.txt            2026-11-23 COM8 实烧原始日志
├── tools/
│   └── gen_gb2312_table.py     GB2312 码表生成脚本（可复现）
└── main/
    ├── CMakeLists.txt
    ├── main.c                  启动顺序：NVS → SYN6288 → TTS 任务 → NimBLE → 广播
    ├── ble_bridge.c/.h         GATT 服务 + 广播（UUID 与串口模块完全一致）
    ├── tts.c/.h                写缓冲拼接、UTF-8→GB2312、分帧播放、Notify 回执
    ├── syn6288.c/.h            协议帧构造、UART1、BY 忙信号
    ├── gb2312.c/.h             UTF-8 → GB2312 转码（二分查表）
    └── gb2312_table.c          7445 项码表（自动生成，勿手改）
```

---

## 2. 接线

| ESP32-S31 | 方向 | SYN6288 模块 | 说明 |
|---|---|---|---|
| **GPIO09** | → | **RXD** | ESP32 的 TX 接模块的 RX |
| **GPIO08** | ← | **TXD** | ESP32 的 RX 接模块的 TX |
| **GPIO12** | ← | **BY** | 忙信号输入：**高=正在合成播放，低=空闲** |
| **3V3** | — | **VCC** | 按用户接线采用 3V3 |
| **GND** | — | **GND** | 必须共地 |

引脚来源：用户提供的接线图片，逐字采用；代码里用
`uart_set_pin(UART_NUM_1, 9, 8, ...)` 显式绑定，不依赖默认脚
（S31 的 `uart_pins.h` 里 U1RXD/U1TXD 均为 `-1`，必须走 GPIO matrix）。

喇叭接模块的 SPK+/SPK−（或模块自带的 3.5mm/排针座）。

---

## 3. 协议（以官方手册为准）

来源：《SYN6288E 中文语音合成芯片 用户手册》，宇音天下
<http://www.doc.voicetx.com/home/SYN6288E>（§4.1 命令帧格式、§4.2 控制命令、§4.4 命令帧举例、§7.3/§9.5 BY 引脚）

### 3.1 帧结构

```
帧头(1)  数据区长度(2)  命令字(1)  命令参数(1)  待发送文本(n≤200)  异或校验(1)
 0xFD     高字节在前      ...
```

- **数据区长度** = 命令字(1) + 命令参数(1，无参数命令则为 0) + 文本(n) + 校验(1)
- **异或校验** = 从 `0xFD` 起到文本最后一个字节，全部按字节异或
- 长度字段的高字节就是 `0x00`，**最高位没有特殊含义**——手册 §4.4 的示例即
  `FD 00 0B 01 01 …`（网上部分文章称 bit15 必须置 1，与手册示例不符，以手册为准）

### 3.2 手册原始示例（本工程可直接对拍）

| 功能 | 帧 |
|---|---|
| 播放 GB2312 "宇音天下" | `FD 00 0B 01 00 D3 EE D2 F4 CC EC CF C2 C1` |
| 播放 GBK "宇音天下" | `FD 00 0B 01 01 D3 EE D2 F4 CC EC CF C2 C0` |
| 设置 9600bps | `FD 00 03 31 00 CF` |
| 设置 19200bps | `FD 00 03 31 01 CE` |
| 设置 38400bps | `FD 00 03 31 02 CD` |
| 停止合成 | `FD 00 02 02 FD` |
| 状态查询 | `FD 00 02 21 DE` |

播放命令参数：**低 3 位 = 文本编码**（0 GB2312 / 1 GBK / 2 BIG5 / 3 UNICODE），
**高 5 位 = 背景音乐编号**（0 为不播 BGM）。本工程用 `0x00`（GB2312、无 BGM）。

### 3.3 帧时序约束（手册 §4.3）

- 帧内字节间隔不能超过 **8ms** → 驱动里整帧拼好后一次性 `uart_write_bytes`
- 帧与帧间隔必须超过 **8ms** → 分帧后 `vTaskDelay(10)`
- 芯片正在合成时若收到新帧，**会立即打断当前合成** → 长文本分帧时必须等上一帧播完
  （本工程用 BY 变低判断），否则前一段会被截断
- 唤醒/改速后需间隔 **16ms** 才能发下一条 → 切波特率后延时 20ms

---

## 4. 编码：为什么必须转 GB2312

SYN6288 只吃 GB2312/GBK 文本，手机 BLE 写进来的是 **UTF-8**。直接下发会乱码或不播报。

ESP-IDF 的 newlib **没有启用 iconv**（实测 `D:\esp\v6.1\esp-idf\components` 下无可用
GBK/GB2312 转换器），所以把码表编译进固件：

- `tools/gen_gb2312_table.py` 用 Python 标准库 `codecs` 的 `gb2312` 编解码器遍历
  `0xA1A1–0xF7FE`，得到 **7445** 个字符，生成
  `(unicode << 16) | gb2312双字节`、按 unicode 升序的 `uint32_t` 数组
- 运行时 `gb2312_from_utf8()` 自己解 UTF-8（含截断/非法序列处理），再二分查表
- 表体积 ≈ 7445 × 4 = **29.8 KB**（源文件 92 KB）

自检方法（已实测）：

```
'你' U+4F60 -> C4 E3      '好' U+597D -> BA C3
'语' U+8BED -> D3 EF      '音' U+97F3 -> D2 F4
与 Python "你好".encode('gb2312') = c4 e3 ba c3 一致
```

**限制**：只覆盖 GB2312 字符集（6763 汉字 + 682 符号）。GB2312 之外的生僻字/emoji
会被跳过并计数，Notify 里显示 `skip<n>`。需要更大字符集时，把生成脚本改成遍历
`0x8140–0xFEFE` 的 GBK 全量（约 21886 项、87 KB），并把播放命令参数改为 `0x01`（GBK）。

---

## 5. 波特率

出厂默认 **9600 bps，8N1，无校验、无流控**（手册 §3.2）。代码用
`SYN6288_BAUD_DEFAULT`（`syn6288.h`）统一控制，改一处即可。

如果模块被改过速、9600 完全没反应，用命令帧 `0x31` 切回去：

```c
syn6288_set_baud_rate(0);   /* 0=9600 1=19200 2=38400 */
```

- 先用**当前**波特率把设置帧发出去，再本地 `uart_set_baudrate`
- 改速后延时 20ms 才能发下一条（手册要求 ≥16ms）
- **每次模块复位都要重发设置帧**（手册：复位后恢复初始波特率）

---

## 6. 编译与烧录

### 6.1 一键烧录（推荐）

双击 `flash.bat` 即可，脚本会自动激活 IDF、自动识别 CH343、固件缺失时先编译：

```bat
flash.bat                  :: 自动找端口 + 烧录
flash.bat -Port COM8       :: 指定端口
flash.bat -Monitor         :: 烧完直接看日志
```

### 6.2 手动烧录

必须在**已激活 IDF v6.1.0 的终端**里执行，且 `esp32s31` 是 preview 目标，`idf.py` 必须带 `--preview`：

```bat
cd /d "C:\Users\yyn\Desktop\ESP32-S31  ks\喇叭"
idf.py --preview set-target esp32s31
set IDF_PY_BUILD_JOBS=3
idf.py --preview build
idf.py --preview -p COMx flash
```

- 本机 `-j` 参数非法，并行度只能用环境变量 `IDF_PY_BUILD_JOBS`（8GB 内存建议 3）
- 烧录口：板载 CH343（VID 1A86 / PID 55D3）
- 镜像偏移：`0x2000` bootloader / `0x8000` partition-table / `0x10000` app，
  模式 `dio / 80m / 16MB`

### 6.3 烧录时要不要拔 SYN6288？

**不需要。** CH343 走的是 UART0（`GPIO58/59`），SYN6288 走 UART1（`GPIO9/8`），
两组引脚互不相干，同时接着也不会冲突。

### 6.4 实烧结果（2026-11-23，COM8）

| 项 | 结果 |
|---|---|
| 芯片 | `ESP32-S31 (revision v0.0)` / MAC `1c:29:04:d0:40:52` |
| `bootloader.bin` @ `0x2000` | No changed sectors found，verified |
| `partition-table.bin` @ `0x8000` | No changed sectors found，verified |
| `laba_s31_tts.bin` @ `0x10000` | 866672 bytes 写入，Hash of data verified |
| 收尾 | `Hard resetting via RTS pin` → `Done` |

原始日志见 `flash_verify.txt`。

### 6.5 为什么串口看不到日志

`console` 被显式让位给了 USB Serial/JTAG（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`、
`CONFIG_ESP_CONSOLE_UART_NUM=-1`），所以 **UART0 上一条日志都不会有**——这是设计，不是故障。
要看日志有两条路：

1. 插板子的原生 USB 口（USB Serial/JTAG），会出现一个 Espressif CDC 端口，日志从那里出；
2. 临时改 `sdkconfig.defaults` 把 console 挪回 UART 再重编，但那样 UART0 会同时跑日志和数据，容易干扰桥接。

---

## 7. 验收

1. 手机 BLE 调试助手扫描并连接 **`kskbl zdjd`**
2. 打开通知（订阅 `6a400003…` 的 CCCD），否则收不到回执
3. 向 `6a400002…` 写入 UTF-8 文本，例如 `你好，我是语音合成模块`
4. 预期：喇叭播报该内容；手机依次收到 `OK:PLAY …` → `ACK:41` → `OK:DONE …ms`
5. 播报期间用万用表/逻辑分析仪测 GPIO12，应为高电平；播完回低

### Notify 消息表（每条 ≤ 20 字节）

| 消息 | 含义 |
|---|---|
| `OK:PLAY <n>B` | 已下发，n = GB2312 字节数 |
| `OK:PLAY <n>B skip<k>` | 同上，另有 k 个字符不在 GB2312 内被跳过 |
| `ACK:41` | 模块回传“命令正确”（0x45 = 命令错误，常见于校验/长度不符） |
| `OK:DONE <n>ms` | 播报结束，耗时 n 毫秒 |
| `WARN:NOBY` | 800ms 内没等到 BY 变高 → 检查 GPIO12 与模块 BY |
| `WARN:TIMEOUT` | 等待播报结束超过估算上限（最长 120s） |
| `ERR:NOGB` | 文本全部无法转 GB2312，未播报 |
| `ERR:UART` | 串口写入失败 |

---

## 8. 不播报时的排查顺序（对应验收反证）

1. **TX/RX 是否交叉**：GPIO09 → 模块 RXD，GPIO08 ← 模块 TXD（接成 TX-TX 是最常见错法）
2. **共地**：ESP32 GND 与模块 GND 必须相连
3. **波特率**：双方 9600 8N1（模块改过速就先发 `0x31` 设置帧）
4. **是否转了 GB2312**：看 Notify 的 `<n>B`，若为 0 或 `ERR:NOGB` 就是编码问题
5. **帧长度与异或**：拿日志里的字节和 §3.2 的手册示例格式对拍；`ACK:45` 基本就是这里错
6. **BY 忙信号**：`WARN:NOBY` 说明 GPIO12 没接到或模块没供电
7. **是否连对设备**：广播名必须是 `kskbl zdjd`（中间一个空格），monitor 会打印本机 BLE MAC
8. **文本是否超 200 字节未分帧**：本工程已自动分帧，若自己改代码要注意这个上限
9. 极端情况：手册 §10.6 的芯片级参考电路提到“上位机发送数据时中间须加反向器”，
   这是裸芯片的接法；市售 SYN6288 模块一般已内置处理，直连即可。若前 8 项都排除且
   模块毫无反应，再核对模块原理图是否属于需要反相的那种

---

## 9. 出处与说明

| 内容 | 来源 |
|---|---|
| 接线（GPIO9/GPIO8/GPIO12、3V3、GND） | 用户提供的接线图片，逐字采用 |
| BLE 广播名 `kskbl zdjd`、三个 128-bit UUID | 本对话上一轮（串口模块）已确定并实机验证 |
| SYN6288 帧格式、命令字/参数、异或、BY 极性、时序约束 | 《SYN6288E 用户手册》§3.2/§4.1/§4.2/§4.3/§4.4/§7.3/§9.5 |
| 波特率 9600 8N1 | 手册 §3.2「初始波特率 9600 bps, 8 数据位, 无校验, 1 停止位」 |
| `esp32s31` 需 `--preview`、必须激活 IDF 终端、`IDF_PY_BUILD_JOBS` | `D:\esp\projects\xiaozhi-esp32-s31\开发环境说明_ESP32-S31.md` §3 |
| GPIO8/9/12 可用、USB PHY 在 GPIO33/34 | `components/soc/esp32s31`：`SOC_GPIO_VALID_GPIO_MASK` 排除 GPIO29/41；`io_mux_reg.h` 的 `USB_INT_PHY0_DM/DP_GPIO_NUM = 33/34` |
| U1 无默认 IOMUX 脚 | `components/soc/esp32s31/include/soc/uart_pins.h`（`U1TXD/U1RXD = -1`） |

**未做实测的部分**：本模块代码编译通过、协议帧与手册示例逐字节对齐，但
**尚未接 SYN6288 实物实烧验证**（语音效果、BY 极性、实际播报时长需上机确认）。

GitHub 上的实测结论以本文件与提交信息为准，不编造未发生的验证。
