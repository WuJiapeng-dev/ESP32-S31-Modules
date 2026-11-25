# 麦克风（Arduino 版）— ESP32-S31 板载麦克风 + BLE 双向 + 离线关键词识别

本目录是 **Arduino IDE 工程**（不是 ESP-IDF 工程），直接用 Arduino IDE 打开 `mic_ble_kws.ino` 即可。

硬件是慧勤智远 **ESP32-S31 CB V1.1 小系统板**，用板载两颗模拟 MEMS 麦克风 + ES8389 编解码器采集，
**不需要外接 INMP441，不需要接任何杜邦线**，只插一根 USB 线。

---

## 1. 环境要求

| 项目 | 要求 |
|---|---|
| Arduino IDE | 2.x（本项目在 Arduino IDE + arduino-cli 1.5.1 下验证） |
| 板卡包 | **arduino-esp32 4.0.0 或更高** |
| 串口监视器 | 115200，编码 UTF-8 |
| USB 驱动 | CH343（板载 USB 转串口），资料包 `7.软件开发工具` 里有 `CH343SER.EXE` |

### 为什么必须 4.0.0

Arduino-ESP32 **4.0.0（2026-11-19 发布）才首次加入 ESP32-S31 支持**，证据是该版本的
`package_esp32_index.json` 里 `toolsDependencies` 明确包含 `esp32 esp32s31-libs 4.0.0`，
而 3.x 全系列没有这一项（3.3.x 的 `tools/` 目录下只有 esp32/s2/s3/c3/c5/c6/h2/p4 的 libs，没有 esp32s31-libs）。

### 安装步骤

1. 打开 Arduino IDE → 文件 → 首选项 → 附加开发板管理器网址，确认有：
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. 开发板管理器 → 搜 `esp32` → 选 **4.0.0** → 安装（约 200–400MB，取决于需下载的工具链）。
3. 装完在「工具 → 开发板」里选 ESP32-S31 对应的板型（4.0.0 的菜单里为 `ESP32S31 Dev Module`；
   若菜单项名称不同，只要芯片选到 S31 即可）。

> 注意：4.0.0 会**替换**掉原来装的 3.3.10-cn，两者不能共存。原来的 S3/C3 工程若要用回 3.3.10，
> 需要重新在开发板管理器里装回 3.3.10。

---

## 2. 编译与烧录

- 打开 `mic_ble_kws.ino`（Arduino 要求 sketch 文件夹名与 .ino 文件名一致且只能字母/数字/下划线开头，本目录 `mic_ble_kws` 已满足；中文文件夹名 IDE 会报"无效的项目名"）。
- 工具 → 端口 → 选 CH343 对应的 COM 口（本项目实测为 COM8）。
- **分区方案建议选 `Huge APP (3MB No OTA/1MB SPIFFS)`**：BLE + 识别代码编译后约 1.03MB，
  默认 1.3MB 的分区余量太少。
- 点上传。上传完成后打开串口监视器（115200），按一下板子复位键。

### 实测编译记录（2026-11-25，arduino-cli 1.5.1 + esp32:esp32 4.0.0）

```
arduino-cli compile --fqbn "esp32:esp32:esp32s31:FlashSize=16M,PartitionScheme=huge_app,USBMode=hwcdc" 麦克风
Sketch uses 1052720 bytes (33%) of program storage space. Maximum is 3145728 bytes.
Global variables use 72568 bytes (22%) of dynamic memory, leaving 255112 bytes.
```

使用的库全部来自 4.0.0 核心自带：`BLE`（Bluedroid 后端）、`Wire`、`Preferences`，无第三方依赖。
板上 IDE 里对应的开发板选项：开发板 `ESP32S31 Dev Module`、Flash Size `16MB`、
Partition Scheme `Huge APP`、USB Mode `Hardware CDC and JTAG`。

> 本版相对上一轮的修复：`kws.h` / `mic_i2s.h` 补了 `<stddef.h>`（`size_t` 未声明），
> 主程序补了 `#include "mfcc.h"`（`MFCC_HOP` 未声明）。三处均为 4.0.0 严格头文件依赖下暴露的真错误。

---

## 3. 硬件与引脚（板载，无外接）

来源：资料包 `2. 原理图/ESP32-S31 CB V1.1_SCH.pdf`、`1. 用户手册` 表 1.1，
并与 `4. 芯片资料/ES8389.PDF`、`LMA3729T381-OAS02.PDF` 逐项核对。

| 信号 | GPIO | 说明 |
|---|---|---|
| I2C SDA (CDATA) | IO0 | ES8389 控制口，7bit 地址 `0x10` |
| I2C SCL (CCLK) | IO1 | 400kHz |
| I2S BCLK (SCLK) | IO3 | ESP32 输出，16k×16bit×2ch = 512kHz |
| I2S WS (LRCK) | IO4 | 帧同步 |
| I2S DIN (ASDOUT) | IO6 | **codec → ESP32**，录音方向 |
| I2S DOUT (DSDIN) | IO5 | ESP32 → codec，本工程不用（不放音） |
| I2S MCLK | IO2 | **本方案不驱动**，时钟由 BCLK 派生 |
| PA_CTRL | IO7 | 两颗 NS4150B 功放使能，高有效；**录音时拉低**，避免喇叭底噪串进来 |

接线对照：**无**。以上全部是板上已连好的走线。

---

## 4. 手机端操作

1. 手机装任意 BLE 调试助手（如 nRF Connect、BLE 调试助手）。
2. 扫描并连接广播名 `kskbl zdjd`（中间一个空格）。
3. 展开服务 `6a400001-b5a3-f393-e0a9-e50e24dcca9e`：
   - **`6a400003-…` 点右侧向下箭头订阅 Notify**（订阅成功才会收到板子的消息）
   - **`6a400002-…` 用于写入文本**（WRITE / WRITE NO RESPONSE 都支持）
4. 可以先发 `PING`，串口会出现 `【手机】PING`，手机收到 `PONG`。

### 支持的 BLE 文本命令

| 命令 | 作用 |
|---|---|
| `PING` | 回 `PONG`，验证链路 |
| `VER` | 回版本号 |
| `STATE` | 回 `mic/codec/模板数/连接状态` |
| `LEARN:0` 或 `LEARN:你好` | 教一句「你好」，下一句话会被存成模板 |
| `LEARN:1` 或 `LEARN:康神` | 教一句「康神开播了，真的假的」 |
| `CLEAR` | 清除所有模板 |
| 其他任意文本 | 原样打印到串口，并回 `已收到：…` |

---

## 5. 关键词识别：方案、依据与边界

### 5.1 支持的两句

1. **你好**
2. **康神开播了，真的假的**

### 5.2 为什么不用 esp-sr / MultiNet

- Arduino-ESP32 的 `ESP_SR` 库依赖 ESP-SR 的**预编译模型与静态库**，而 Arduino-ESP32 3.x 的
  `tools/` 下根本没有 `esp32s31-libs`；4.0.0 才补上 `esp32s31-libs`，但 ESP-SR 官方的
  WakeNet/MultiNet 模型针对 S31 的可用性没有发布说明背书，属于"装了可能编不过、过了也可能跑不起来"的路径。
- 更早用 ESP-IDF 做过同类评估：IDF v6.1 内置 147 个组件里没有 esp-sr，且 whisper-tiny（约 40MB）、
  vosk small-cn（约 42MB）远超本芯片可用 RAM（实测剩 335KB）。**所以大模型类 ASR 在这块板子上不可行。**

结论：**离线通用语音识别不可行，但"固定小词表关键词识别"可行**，本工程就做后者。

### 5.3 实际实现（两条路径，互为兜底）

**路径 A — 声学特征分类（默认，开箱即用，不需要先教）**

1. **端点检测**：每 10ms 取 160 个样本，算短时 RMS；噪声底用 `0.95/0.05` 一阶平滑自适应跟踪，
   触发阈值 `max(3×噪声底, 180)`，连续 3 帧过阈才算开口，连续 250ms 静音算说完，带 300ms 预滚防吃字头。
2. **音节峰计数**：对这句话的 RMS 包络做 3 点平滑，找局部极大值（阈值 `0.18×峰值`），
   相邻峰至少间隔 60ms 才算一个新音节。
3. **联合判决**：
   - 音节 ≥ 5 或时长 ≥ 1.3s → 「康神开播了，真的假的」
   - 音节 ≤ 3 且时长 ≤ 1.1s → 「你好」
   - 其余 → 打印「未匹配」，**不猜**

这两句话在声学上差得非常远（2 音节/约 0.5s vs 8 音节/约 2s），所以这条路径对它们是可靠的；
但它**不具备泛化能力**——换成别的两句话就要重做判决规则。

**路径 B — MFCC + DTW 模板匹配（可选增强）**

- 特征：25ms 帧长 / 10ms 帧移 / 512 点 FFT / 26 带 Mel 三角滤波 / 取 13 维倒谱（丢弃 c0），
  再做倒谱均值归一（CMN）抹掉通道差异。
- 比对：DTW 带 Sakoe-Chiba 带宽约束，防止"横着乱对齐"得出虚假小距离；距离按路径长度归一化，
  阈值 20.0。
- 模板存 NVS（Preferences），掉电不丢，最多两句各一条。
- 教过之后同一句话的 DTW 距离会明显更低，判决优先走模板；没教过的句子仍回落到路径 A。

> 说明：所有算法都在本目录内自包含实现（`fft.cpp` / `mfcc.cpp` / `dtw.cpp` / `kws.cpp`），
> 不依赖任何第三方库，也不联网。

### 5.4 明确的边界（避免误解）

- 这不是"听懂任意中文"，只是**对两句固定短语做声学判别**。
- 环境噪音很大、说话离板子很远、或者语速极端时，可能判成「未匹配」——此时不会乱打印。
- 想要更稳，按第 4 节先对每句教一遍（`LEARN:0` / `LEARN:1`），之后走 DTW。

---

## 6. 串口输出样例

```
==== ESP32-S31 板载麦克风 + BLE 双向 + 离线关键词识别 ====
固件：麦克风(Arduino)/1.0   串口：115200 UTF-8
[I2S] 已启动：BCLK=GPIO3 WS=GPIO4 DIN=GPIO6，16000Hz 16bit 立体声，BCLK=512kHz，MCLK 不驱动
[ES8389] I2C 探测到器件（SDA=0 SCL=1 7bit 0x10）
[ES8389] 回读 0x05=0x55 0x06=0x84 0x07=0xD0 0x09=0xC1 0x02=0x40
[ES8389] 麦克风 PGA 设为 30.0dB（0x72=0x1C）
[I2S] 电平自检：左 RMS=98 峰值=214 | 右 RMS=91 峰值=203（RMS 接近 0 说明没采到数据）
[识别] 就绪：0 条模板，走声学特征分类（无需训练）
[蓝牙] 开始广播，设备名 "kskbl zdjd"
【蓝牙】手机已连接
【手机】PING
【开发板】PONG
【识别】命中「你好」（58 帧 / 2 音节 / DTW=-1.0）
【开发板】识别到：你好
【手机】LEARN:0
【识别】请现在说：「你好」
【识别】模板已保存：「你好」62 帧
```

回归自检方法：

| 现象 | 检查点 |
|---|---|
| 静音时串口一直冒出「命中」 | 说明是假识别 → 看 `【识别】未匹配` 的帧数，检查噪声底是否被空调/风扇抬高 |
| 说话但串口什么都没有 | 先发串口命令 `lvl` 看 RMS 是否接近 0；接近 0 就是 I2S/ES8389 没通 |
| 串口中文乱码 | 串口监视器编码要选 UTF-8、波特率 115200 |
| 手机搜不到 `kskbl zdjd` | 检查板卡包是不是 4.0.0、CH343 驱动、以及固件是否真的烧进去了 |
| 手机连上但收不到通知 | 没订阅 `6a400003` 的 Notify |

---

## 7. 目录说明

| 文件 | 作用 |
|---|---|
| `mic_ble_kws.ino` | 主程序：初始化顺序、串口/BLE 命令、主循环 |
| `es8389.h/.cpp` | ES8389 录音链路初始化（寄存器序列移植自 esp_codec_dev 1.6.2） |
| `mic_i2s.h/.cpp` | I2S 主机端采集 + 电平自检 |
| `kws.h/.cpp` | 端点检测 + 音节计数分类 + DTW 模板管理 |
| `mfcc.h/.cpp` | 13 维 MFCC |
| `fft.h/.cpp` | 512 点基 2 FFT |
| `dtw.h/.cpp` | DTW（Sakoe-Chiba 带宽约束） |
| `ble_bridge.h/.cpp` | GATT 服务 + 通知 |

---

## 8. 出处

- Arduino-ESP32 4.0.0 支持 S31：Espressif `package_esp32_index.json` 中 4.0.0 的
  `toolsDependencies` 含 `esp32s31-libs 4.0.0`（版本发布日期 2026-11-19）。
- 板载麦克风与引脚：慧勤智远 `ESP32-S31 CB V1.1_SCH.pdf`、用户手册表 1.1、`ES8389.PDF`、`LMA3729T381-OAS02.PDF`。
- ES8389 寄存器序列：`esp_codec_dev` 1.6.2 的 `device/es8389/es8389.c`（`es8389_open` 等六个函数）。
- BLE 参数（服务/特征 UUID、广播名）：沿用本仓库旧模块的既定约定。
- MFCC / DTW 参数：HTK/Kaldi 常规取值（25ms 帧长、10ms 帧移、26 带 Mel、13 维倒谱）。
