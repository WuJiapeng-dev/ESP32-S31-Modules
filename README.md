# ESP32-S31 模块仓库

ESP32-S31 系列功能模块的**多模块汇总仓库**。一个模块一个目录，各自是独立的 ESP-IDF 工程，
互不依赖、可单独编译烧录。

> 本仓库不是单一工程仓库：根目录只放总说明，**模块代码一律在自己的子目录里**。
> 后续新增模块直接在根下开新目录，不影响已有模块。

---

## 1. 模块清单

| # | 目录 | 模块名 | 功能 | 板级外设 | 状态 |
|---|---|---|---|---|---|
| 01 | [`chuankou/`](chuankou/) | 串口/BLE 桥 | BLE 收 `ON`/`OFF` → UART0 转发 → STM32 控 LED，Notify 回执 | UART0 (GPIO58/59) + NimBLE | 已烧录验证（CH343/COM8，2026-11-23） |
| 02 | [`laba/`](laba/) | SYN6288 语音合成 | BLE 收任意 UTF-8 文本 → 转 GB2312 → UART1 下发 SYN6288 播报，BY 忙信号回执 | UART1 (GPIO9/8) + BY (GPIO12) + NimBLE | 编译通过（0 error / 0 warning），待实机验证 |
| 03 | [`laba_cloud/`](laba_cloud/) | 云端语音合成 | BLE 收文本 → HTTPS 调火山 TTS 拿 PCM → I2S 推功放；音色/语速/音量网页可调 | I2S (GPIO15/16/17) + Wi-Fi + NimBLE | 编译通过（0 error / 0 warning，1.64MB），待实机验证 |

三个模块**共用同一套 BLE 身份**：广播名 `kskbl zdjd`，服务/特征 UUID
`6a400001/0002/0003-b5a3-f393-e0a9-e50e24dcca9e`。手机调试助手的配置可以原样复用，
刷哪个模块就连哪个（同一时间只能跑一个）。

`laba` 与 `laba_cloud` 是同一件事的两条路：SYN6288 音色固定（芯片只固化一个发音人，
音频模拟直出、ESP32 拿不到数字流），要换音色只能走云端 TTS，于是另开一个模块，
原模块保留作离线兜底。两者引脚不冲突，可以同时接线，但固件只能刷一个。

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
├── .gitignore             ← 仓库级忽略规则（覆盖所有模块）
└── <模块名>/               ← 一个模块一个目录，目录名即模块名
    ├── README.md          ← 模块说明：协议/接线/编译/验收/排查
    ├── CMakeLists.txt     ← ESP-IDF 工程入口（必须在这一层）
    ├── sdkconfig.defaults ← 模块的默认配置，随仓库提交
    └── main/              ← 源码
```

约定：

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

1. 根目录建新文件夹，名字用英文小写（如 `wifi_relay`）
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
