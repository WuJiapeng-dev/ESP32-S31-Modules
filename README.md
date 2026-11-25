# ESP32-S31 模块仓库

ESP32-S31 系列功能模块的**多模块汇总仓库**。一个模块一个目录，各自是独立的 ESP-IDF 工程，
互不依赖、可单独编译烧录。

> 本仓库不是单一工程仓库：根目录放**总说明 + 对外展示的网页**，
> **模块代码一律在自己的子目录里**。后续新增模块直接在根下开新目录，不影响已有模块。

---

## 1. 模块清单

| # | 目录 | 模块名 | 功能 | 板级外设 | 状态 |
|---|---|---|---|---|---|
| 01 | [`串口/`](串口/) | 串口/BLE 桥 | BLE 收 `ON`/`OFF` → UART0 转发 → STM32 控 LED，Notify 回执 | UART0 (GPIO58/59) + NimBLE | 已烧录验证（CH343/COM8，2026-11-23） |
| 02 | [`喇叭/`](喇叭/) | SYN6288 语音合成 | BLE 收任意 UTF-8 文本 → 转 GB2312 → UART1 下发 SYN6288 播报，BY 忙信号回执 | UART1 (GPIO9/8) + BY (GPIO12) + NimBLE | 已烧录验证（COM8，2026-11-23），待实物验证播报 |
| 02a | [`喇叭/复述/`](喇叭/复述/) | 语音复述 | INMP441 → 离线自学习识别（MFCC+DTW）→ BLE Notify + SYN6288 念出识别结果 | I2S (GPIO19/46/13) + UART1 (GPIO9/8) + BY (GPIO12) + NimBLE | 编译通过（0 error / 0 warning，0.89MB，分区余 78%），已烧录，待实物验证 |
| 02b | [`喇叭/手机接收/`](喇叭/手机接收/) | 识别送手机 | INMP441 → 离线自学习识别 → **只经 BLE Notify 推文字给手机**，不接喇叭（无 UART/TTS） | I2S (GPIO19/46/13) + NimBLE | 编译通过（0 error / 0 warning），待实机验证 |
| 03 | [`音色/`](音色/) | 云端语音合成 | BLE 收文本 → HTTPS 调 **DeepSeek TTS** 拿裸 PCM → I2S/PDM 推功放；网页配 Key、选引擎与音色 | I2S (GPIO15/16/17) 或 PDM (GPIO17) + Wi-Fi + NimBLE | 编译通过（0 error / 0 warning，1.72MB，分区余 59%），网页见 [`控制台.html`](控制台.html)，待实机验证 |

> `喇叭/` 下现在是**三个互相独立的 ESP-IDF 工程**：根目录（SYN6288 离线合成）、
> `复述/`（麦克风识别 + 喇叭念出来）、`手机接收/`（麦克风识别 → 手机显示，不接喇叭）。
> 每层都有自己的 `CMakeLists.txt` / `sdkconfig.defaults` / `main/`，可单独编译烧录，
> 互不依赖。同一块板子同一时间只能跑一个。

> **模块 03 的目录名有个硬限制**：ESP-IDF 的工具链处理不了非 ASCII 路径，
> 在 `音色/` 这种中文目录下 `build` 会失败（gcc 报 `cannot read spec file`，
> 中文被转成了换行符）。要编译得把工程放到纯英文路径，详见 `音色/README.md` §7.3。

> 目录名用中文，GitHub 网页上的链接会自动转成 `%E4%B8%B2%E5%8F%A3` 这类 percent-encoded
> 形式，属正常现象，点进去照常能打开。`git clone` 到本地后目录名就是中文。

五个工程**共用同一套 BLE 身份**：广播名 `kskbl zdjd`，服务/特征 UUID
`6a400001/0002/0003-b5a3-f393-e0a9-e50e24dcca9e`。手机调试助手的配置可以原样复用，
刷哪个工程就连哪个（同一时间只能跑一个）。

`喇叭` 与 `音色` 是同一件事的两条路：SYN6288 音色固定（芯片只固化一个发音人，
音频模拟直出、ESP32 拿不到数字流），要换音色只能走云端 TTS，于是另开一个模块，
原模块保留作离线兜底。两者引脚不冲突，可以同时接线，但固件只能刷一个。

**识别能力要看清（`喇叭/复述` 与 `喇叭/手机接收` 用的是同一个内核）**：
它是"自学习模板匹配"，只能认出你用 `EN:` 教过的句子（最多 8 条、单句 2 秒），
**不是通用中文听写**。原因是芯片没有 esp-sr 的预编译库、whisper/vosk 那类模型
又远超 RAM，详细依据见 `喇叭/手机接收/README.md` §1。
要认任意中文得走云端 ASR（两个工程都在 `on_utterance_done()` 留了接入点）。

后续模块在此表追加一行。

## 2. 环境要求

| 项 | 值 |
|---|---|
| 芯片目标 | `esp32s31`（**ESP-IDF preview 目标**，所有 `idf.py` 必须带 `--preview`） |
| ESP-IDF | v6.1.0 |
| 开发板 | ESP32-S31-CB V1.1（模块 ESP32-WROOM-3） |
| 烧录口 | 板载 USB 转串口 **CH343**（VID `1A86` / PID `55D3`，枚举为 `USB-Enhanced-SERIAL CH343`），串口下载模式 `UART` |

`esp32s31` 在 v6.1 里是 preview 目标，**漏掉 `--preview` 会直接报 target 不支持**。

## 3. 目录约定

```
ESP32-S31-Modules/
├── README.md              ← 本文件：仓库总说明、模块清单、环境约定
├── 控制台.html             ← 对外展示：模块 03 的网页控制台预览（离线，数据是假的）
├── .gitignore             ← 仓库级忽略规则（覆盖所有模块）
├── 串口/                   ← 模块 01：BLE ↔ UART0 桥
├── 喇叭/                   ← 模块 02：SYN6288 离线语音合成（根目录这一层就是一个独立工程）
│   ├── CMakeLists.txt     ← 根工程的入口
│   ├── main/              ← BLE 收文本 → GB2312 → SYN6288 播报
│   ├── 复述/               ← 02a：INMP441 → 识别 → Notify + 喇叭念出来
│   │   ├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
│   │   └── main/          ← mic_i2s / fft / mfcc / dtw / asr + syn6288 / gb2312 / tts
│   └── 手机接收/           ← 02b：INMP441 → 识别 → **只发 BLE**，不接喇叭
│       ├── CMakeLists.txt / sdkconfig.defaults / partitions.csv
│       └── main/          ← mic_i2s / fft / mfcc / dtw / asr（无 uart / 无 tts / 无 gb2312）
├── 音色/                   ← 模块 03：云端 TTS（DeepSeek → I2S）
│   ├── README.md          ← 模块说明：协议/接线/编译/验收/排查
│   ├── CMakeLists.txt     ← ESP-IDF 工程入口（必须在这一层）
│   ├── sdkconfig.defaults ← 模块的默认配置，随仓库提交
│   ├── tools/
│   │   └── gen_preview.py ← 从 wifi_prov.c 抽出 PAGE_HTML，同时写根目录 控制台.html
│   └── main/              ← 源码
```

约定：

- **网页这类展示物料放根目录，只有代码分模块**；`控制台.html` 由
  `音色/tools/gen_preview.py` 生成，不要手改（改 `音色/main/wifi_prov.c` 后重跑脚本）
- **目录名即模块名，用中文**；目录内部的文件名、工程名（`project()`）仍保持 ASCII，
  避免工具链在非 ASCII 路径下的意外问题
- **不放 `sdkconfig`**（构建时生成，机器相关），只提交 `sdkconfig.defaults`
- **不放 `build/`**、`*.bin`、`*.elf`、`*.map`（见 `.gitignore`）
- 每个模块的 `README.md` 必须能独立说明：协议、引脚、构建命令、验收标准、排查顺序
- 模块之间不共享代码；确需共享时再单独开 `common/` 并在各模块 `CMakeLists.txt` 里引用

## 4. 克隆与构建

```bat
git clone https://github.com/WuJiapeng-dev/ESP32-S31-Modules.git
cd ESP32-S31-Modules\<模块名>
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p COMx flash monitor
```

必须在**已激活 ESP-IDF v6.1.0 的终端**里执行；普通 PowerShell / Git Bash 未激活环境会失败。
内存紧张的机器用环境变量限制并行度（`-j` 参数在 v6.1 非法）：

```bat
set IDF_PY_BUILD_JOBS=3
idf.py --preview build
```

## 5. 新增模块步骤

1. 根目录建新文件夹，名字用中文（与 `串口`/`喇叭`/`音色` 一致）
2. 拷一份现有模块的 `CMakeLists.txt` / `main/CMakeLists.txt` 作骨架，改 `project()` 名
3. 写自己的 `sdkconfig.defaults`（**别忘 console 配置**，见下）
4. 写上自己的 `README.md`
5. 更新本文件 §1 的模块清单表
6. `idf.py --preview set-target esp32s31` → `idf.py --preview build` 确认能过
7. `git add . && git commit -m "feat(<模块名>): 新增 xxx 模块"`

## 6. 全局注意事项

- **UART0 与 console**：UART0 默认被 IDF 日志占用。凡是把 UART0 当业务串口用的模块，
  必须在 `sdkconfig.defaults` 里把 console 搬到 USB Serial/JTAG：
  ```ini
  CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
  CONFIG_ESP_CONSOLE_UART_DEFAULT=n
  ```
  依据：`SOC_USB_SERIAL_JTAG_SUPPORTED = 1`（`soc/esp32s31/include/soc/soc_caps.h`）。
- **GPIO 合法范围**：`SOC_GPIO_PIN_COUNT = 62`，即 GPIO0–61；GPIO29 / GPIO41 不可用。
- **U1/U2/U3 没有默认引脚**：`soc/esp32s31/include/soc/uart_pins.h` 里 `U1RXD/U1TXD`
  等全是 `-1`，所以用 UART1 及以上必须显式 `uart_set_pin()`（走 GPIO matrix，任意合法脚都行）。
  用 UART1 的好处是天生不撞 console，不必改 console 配置。
- **USB PHY 占用 GPIO33/34**（`io_mux_reg.h` 的 `USB_INT_PHY0_DM/DP_GPIO_NUM`），
  这两个脚不能挪作他用。
- **NimBLE API 差异**：v6.1 里 `ble_svc_gap_init()` / `ble_svc_gatt_init()` 返回 `void`（不能接返回值）；
  `ble_store_config_init()` 需应用侧显式调用。

## 7. 提交规范

```
feat(<模块>):  新增功能
fix(<模块>):   修 bug
docs(<模块>):  只改文档
chore:         仓库级杂项（.gitignore、README 等）
refactor:      结构调整
```

## 8. 许可

未声明。如需开源请自行添加 `LICENSE`。
