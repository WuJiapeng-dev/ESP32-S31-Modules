# 模块 01 · chuankou —— BLE ↔ UART0 桥

手机 BLE 调试助手连上广播名 **`kskbl zdjd`**，向 Write 特征写 ASCII `ON` / `OFF`，
ESP32-S31 通过 **UART0（TX=GPIO58 / RX=GPIO59）** 把 `ON\n` / `OFF\n` 发给对端 MCU，
并通过 Notify 回执 `OK:ON` / `OK:OFF` 便于手机确认执行结果。

本模块只做**协议转换**：BLE GATT 一进一出，UART0 一行一发。对端逻辑（拉 LED、驱动继电器等）由对端实现。

---

## 1. 目录结构

```
chuankou/
├── README.md              ← 本文件
├── CMakeLists.txt         ← ESP-IDF 工程入口（必须在这一层）
├── sdkconfig.defaults     ← console 搬 USB Serial/JTAG、NimBLE、Flash 16MB
├── .vscode/settings.json  ← 固定 IDF_TARGET=esp32s31
└── main/
    ├── CMakeLists.txt
    ├── main.c             ← app_main：NVS → UART0 → NimBLE → host 任务
    ├── ble_bridge.c/.h    ← GATT 服务（6a400001/2/3）、广播、Notify 回执
    └── uart_link.c/.h     ← UART0 显式引脚绑定、发送、接收行解析
```

## 2. BLE 协议

| 角色 | UUID | 属性 |
|---|---|---|
| Service (Primary) | `6a400001-b5a3-f393-e0a9-e50e24dcca9e` | — |
| Write 特征（手机→ESP32） | `6a400002-b5a3-f393-e0a9-e50e24dcca9e` | WRITE \| WRITE NO RESPONSE |
| Notify 特征（ESP32→手机） | `6a400003-b5a3-f393-e0a9-e50e24dcca9e` | NOTIFY（CCCD `00002902-0000-1000-8000-00805f9b34fb` 由协议栈自动生成） |

- 广播名：`kskbl zdjd`（含 1 个空格，共 9 个字符，逐字符一致）
- 广播方式：扩展广播 API + `legacy_pdu = 1`（等价 ADV_IND），兼容不支持 BLE5 扩展包的老手机；
  扫描响应里带 128-bit 服务 UUID，方便调试助手识别
- 不要求配对/加密，手机可直接写（调试场景）

## 3. 指令与 UART0 协议

| 手机写入（Write） | UART0 发出 | Notify 回执 |
|---|---|---|
| `ON` | `ON\n` | `OK:ON` |
| `OFF` | `OFF\n` | `OK:OFF` |
| 其它任何内容 | 不转发 | 不回执（忽略） |

- 收到内容自动去首尾空白 / `\r\n`，比较**不分大小写**（`on` 等价 `ON`）
- 长度必须严格相等，`ONX` 不会被当成 `ON`
- UART0 参数：**115200 / 8N1 / 无流控**
- **对端要求**：按 `\n`（或 `\r`）切行，整行等于 `ON` / `OFF` 才动作；行解析请用中断或独立任务，不要在主循环阻塞轮询

## 4. 引脚与接线

| ESP32-S31 | 方向 | 对端 MCU | 说明 |
|---|---|---|---|
| GPIO58 (U0TXD) | → | RX | **交叉**：TX 接 RX |
| GPIO59 (U0RXD) | ← | TX | **交叉**：RX 接 TX |
| GND | — | GND | **必须共地**，否则串口必挂 |
| 3.3V（可选） | — | 3.3V | 独立供电时也要共地 |

- 两边都是 3.3V TTL，**可直连，不要接 5V**
- 代码里用 `uart_set_pin(UART_NUM_0, 58, 59, ...)` 显式绑定，不依赖芯片默认脚
- GPIO58/59 合法性：`SOC_GPIO_PIN_COUNT = 62`，且这两脚不在 `SOC_GPIO_VALID_GPIO_MASK` 的屏蔽位里

## 5. 编译

`esp32s31` 是 ESP-IDF 的 **preview 目标**，且必须在本机已激活 IDF v6.1.0 的终端里执行
（普通 PowerShell / Git Bash 未激活环境会失败）。

```bat
:: 打开桌面快捷方式「IDF_v6.1_Powershell」
cd /d <仓库路径>\chuankou
idf.py --preview set-target esp32s31
idf.py --preview build
```

内存紧张（8GB 机器）用环境变量限制并行度，`-j` 参数不合法：

```bat
set IDF_PY_BUILD_JOBS=3
idf.py --preview build
```

若 PowerShell 执行策略是 Restricted，在当前进程内临时放开再激活：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. '<IDF 安装路径>\Microsoft.v6.1.PowerShell_profile.ps1'
```

## 6. 烧录与看日志

**一键烧录（推荐）**：双击 `flash.bat`

它会自动完成：激活 IDF v6.1.0 → 查找 CH340 端口（优先 `PID_7523`）→ 无固件则先编译 → 烧录。
可选参数：

```bat
flash.bat                  :: 自动找端口 + 烧录
flash.bat -Port COM7       :: 指定端口
flash.bat -Monitor         :: 烧完直接进 monitor（Ctrl+] 退出）
```

端口没找到时脚本会直接列出排查项（UART 口 / 数据线 / CH341SER 驱动），不会盲烧。

**手动烧录**

```bat
cd /d <仓库路径>\chuankou
idf.py --preview -p COMx flash
idf.py --preview -p COMx monitor
```

- 板载 USB 转串口实测为 **CH343**（VID `1A86` / PID `55D3`，枚举成 `USB-Enhanced-SERIAL CH343 (COMx)`）；
  本机历史环境里也出现过 CH340（`PID_7523`）的端口，`flash.bat` 两种都认。插板后在设备管理器确认 COM 号
- 日志走 **USB Serial/JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）；
  若该口引到了板上的另一个 USB 口，日志就从那个口看，CH340 口只负责烧录
- 退出 monitor：`Ctrl+]`
- 烧录偏移（`build/flash_args`）：`0x2000` bootloader / `0x8000` 分区表 / `0x10000` app，
  `dio` / `80m` / `16MB`

## 7. 为什么 console 必须搬走（关键）

UART0 默认被 IDF console 日志占用。不改的话每一行 `ESP_LOGI` 都会从 TX0 灌进对端 RX，
对端收到的是日志垃圾而不是 `ON`/`OFF`。本模块已在 `sdkconfig.defaults` 落实：

```ini
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
```

依据：本芯片 `SOC_USB_SERIAL_JTAG_SUPPORTED = 1`
（`components/soc/esp32s31/include/soc/soc_caps.h`）。生成后的 `sdkconfig` 里
`CONFIG_ESP_CONSOLE_UART_NUM = -1`，即 UART console 已彻底关闭。

> 残余噪声：复位瞬间 ROM bootloader 仍可能往 UART0 打极短一段，发生在 App 启动前、
> 无法用 Kconfig 消除；对端按行解析会直接忽略。

## 8. 验收标准

1. 上电，手机 BLE 调试助手扫描到 **`kskbl zdjd`** 并连接
2. 展开服务确认 `6a400001-…`；`…0002` 可写，`…0003` 打开 Notify（写 CCCD `0100`）
3. 写 ASCII `ON` → 对端执行动作（如 LED 亮），Notify 收到 `OK:ON`
4. 写 `OFF` → 对端恢复（LED 灭），Notify 收到 `OK:OFF`
5. 写 `hello` 等其它内容 → 无转发、无回执

## 9. 排查顺序（按这个顺序查，别跳）

1. **共地**：ESP32 与对端 GND 必须连一起
2. **交叉接**：GPIO58 → 对端 RX；GPIO59 ← 对端 TX（接反最常见）
3. **波特率一致**：两边都 115200 8N1
4. **UART0 是否让出**：menuconfig → Component config → ESP-STDIO → 必须是 `USB Serial/JTAG Controller`
5. **对端是否在跑**：给对端上电应能自己吐一行启动信息，收不到说明 TX 方向就不通
6. **BLE 连对设备**：周围可能有同名设备，看 monitor 打印的本机 BLE 地址
7. **引脚是否被占用**：GPIO58/59 若被其它模块复用，UART 会静默失败

## 10. 出处与依据

| 结论 | 出处 |
|---|---|
| `esp32s31` 是 preview 目标、`idf.py` 必须带 `--preview`、须在激活的 IDF 终端跑 | 本机 `开发环境说明_ESP32-S31.md` §3 |
| IDF 版本 v6.1.0 / 工具链路径 | 同上 §1 |
| BLE 服务与特征 UUID、属性 | 项目委托人提供的蓝牙调试助手截图 |
| 广播名 `kskbl zdjd`（含空格） | 项目委托人明确指定 |
| UART0 TX0=GPIO58 / RX0=GPIO59 | 项目委托人明确指定；**交叉验证**：`soc/esp32s31/include/soc/uart_pins.h` 的 `U0TXD_GPIO_NUM 58` / `U0RXD_GPIO_NUM 59` 与之一致 |
| console 可改 USB Serial/JTAG | `components/esp_stdio/Kconfig` + `SOC_USB_SERIAL_JTAG_SUPPORTED=1` |
| GPIO58/59 合法 | `soc_caps.h`：`SOC_GPIO_PIN_COUNT=62` |
| Flash 16MB | 环境说明 §4 `flash_project_args` 的 `--flash-size 16MB` |

## 11. 构建验证记录（2026-11-23）

环境：ESP-IDF **v6.1.0**，目标 `esp32s31`（preview），`IDF_PY_BUILD_JOBS=3`。

| 项 | 结果 |
|---|---|
| `idf.py --preview set-target esp32s31` | 成功 |
| `idf.py --preview build` | `Project build complete`，**0 error / 0 warning** |
| `kskbl_s31_bridge.bin` | `0xcaa50` = 811 KB（1 MB app 分区，剩 21%） |
| `bootloader.bin` | `0x4cc0` |
| `sdkconfig` 关键项 | `ESP_CONSOLE_USB_SERIAL_JTAG=y`、`ESPTOOLPY_FLASHSIZE="16MB"`、`NIMBLE_ROLE_PERIPHERAL/BROADCASTER=y`、`NIMBLE_EXT_ADV=y` |
| 实烧验证 | **已烧录（2026-11-23 12:31）**，见下节 |

### 实烧结果（CH343 / COM8）

```
idf.py --preview -p COM8 flash
```

```
Connected to ESP32-S31 on COM8:
Chip type:          ESP32-S31 (revision v0.0)
Features:           Wi-Fi 6, BT 5.4 (LE), IEEE802.15.4, Dual Core + LP Core, 300MHz
Crystal frequency:  40MHz
MAC:                1c:29:04:d0:40:52

Writing 'bootloader/bootloader.bin' at 0x00002000...      verified
Writing 'partition_table/partition-table.bin' at 0x00008000... verified
Writing 'kskbl_s31_bridge.bin' at 0x00010000...            verified
Hard resetting via RTS pin...  Done
```

- 三段镜像（bootloader `0x2000` / 分区表 `0x8000` / app `0x10000`）**全部 `verified`**
- 二次执行时 esptool 报 `No changed sectors found, verifying if data is in flash`，
  说明内容与 flash 中完全一致（逐扇区核对通过），全程 13 秒
- **运行态未从串口验证**：模块把 console 改到了 USB Serial/JTAG，CH343（UART0）上不会有启动日志，
  复位后 1.8 秒内串口无数据属预期。运行验收需用手机 BLE 助手搜 `kskbl zdjd`，或把板子的原生 USB 口
  接到电脑看日志（会枚举成 `VID_303A` 的 USB 串行设备）

> **接线注意（实测发现）**：CH343 就挂在 UART0（即 GPIO58/59）上。烧录完成后如果还要接 STM32 的
> PA2/PA3，建议**先拔掉 CH343 这根 USB 线**——否则 CH343 的 TX 与 STM32 的 TX 会同时驱动 GPIO59，
> 两个推挽输出对顶，轻则通信乱码，重则伤引脚。

**编译期已踩过的坑（都已修）**

1. IDF v6.1 的 `ble_svc_gap_init()` / `ble_svc_gatt_init()` 返回 **void**，不能接返回值
2. `ble_store_config_init()` 需应用侧显式调用，否则 `store_status_cb` 无实现
3. 8GB 内存 + 安全软件环境下首次全量编译可能卡死（`bootloader configure` 或 ELF 链接阶段，
   进程 CPU 不再增长）：结束 `ninja/cmake/python` 后原地重跑增量即可，或把 `IDF_PY_BUILD_JOBS` 降到 2
