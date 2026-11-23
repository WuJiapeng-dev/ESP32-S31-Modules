# ESP32-S31 模块仓库

ESP32-S31 系列功能模块的**多模块汇总仓库**。一个模块一个目录，各自是独立的 ESP-IDF 工程，
互不依赖、可单独编译烧录。

> 本仓库不是单一工程仓库：根目录只放总说明，**模块代码一律在自己的子目录里**。
> 后续新增模块直接在根下开新目录，不影响已有模块。

---

## 1. 模块清单

| # | 目录 | 模块名 | 功能 | 板级外设 | 状态 |
|---|---|---|---|---|---|
| 01 | [`chuankou/`](chuankou/) | 串口/BLE 桥 | BLE 收 `ON`/`OFF` → UART0 转发 → STM32 控 LED，Notify 回执 | UART0 (GPIO58/59) + NimBLE | 编译通过，待实烧 |

后续模块在此表追加一行。

## 2. 环境要求

| 项 | 值 |
|---|---|
| 芯片目标 | `esp32s31`（**ESP-IDF preview 目标**，所有 `idf.py` 必须带 `--preview`） |
| ESP-IDF | v6.1.0 |
| 开发板 | ESP32-S31-CB V1.1（模块 ESP32-WROOM-3） |
| 烧录口 | 板载 USB 转串口 CH340（VID `1A86` / PID `7523`），串口下载模式 `UART` |

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
