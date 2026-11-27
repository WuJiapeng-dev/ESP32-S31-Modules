# 编译环境说明（务必先读这一份）

本工程代码已完成，但**在本机当前环境下没能跑完一次完整编译**。
这份文档记录踩到的三个真实坑、已经做的修复、以及你自己怎么把它编出来。

不是"代码写不出来"，而是这台机器的 IDF 构建链在每一处外部命令上都被拖到不可用：

| 环节 | 现象 | 根因 | 处理 |
|---|---|---|---|
| cmake 配置 | 打完 `-- Found Git:` 就不动了，10~40 分钟无任何输出 | `D:\esp\v6.1\esp-idf` 目录的 `git describe --tags` 挂死（手动执行同样挂，`timeout 20` 也拉不回来），IDF 的 `build.cmake:146` 会无条件调它拿版本号 | 临时把 `esp-idf\.git` 改名，让 git 立刻报 `not a git repository` 并回落到 `IDF_VERSION_MAJOR/MINOR`。编译完记得改回去 |
| cmake 配置 | 到了 `-- Checking Python dependencies...` 又停住 | `idf_tools.py check-python-dependencies` 要联网核对 pip 包版本 | 加 `-DPYTHON_DEPS_CHECKED=1`（`project.cmake:138` 有这个开关，会自动把 `__CHECK_PYTHON` 置 0） |
| cmake 配置 | 子模块检查处长时间无响应 | `git_submodules.cmake` 发现缺失子模块会跑 `git submodule update --init --recursive` 联网拉取 | 环境变量 `IDF_SKIP_CHECK_SUBMODULES=1` |
| 编译器探测 | `Detecting C compiler ABI info` 等每一步要 2~4 分钟 | 每个新生成的 `riscv32-esp-elf-g++.exe` / `cc1plus.exe` 进程都被本机安全软件深度扫描一次；一次配置要做约 20 次 try_compile | 无法从工程侧解决，只能把 IDF 工具链目录加进安全软件白名单，或换一台机器 |
| bat 脚本 | 变量全部没设上，报 `'ESP32-S31' 不是内部或外部命令` | cmd 即使 `chcp 65001` 也会把 UTF-8 中文注释的 bat 撕碎，然后逐行当命令执行 | 两个 bat **全部改纯 ASCII**，中文说明只写在 md 里 |

最后一条是决定性的：前面几项修完之后，配置仍卡在编译器探测，把整条链的时间拉到小时级，所以本次没有拿到 `xiaozhi_ai.bin`。

## 你可以怎么把它编出来

1. 把 `C:\Espressif\tools` 整个目录加进安全软件（火绒 / 360 / Defender）的排除列表。这一步是关键。
2. 在纯英文路径编译（本机 `ESP32-S31  ks` 有中文和双空格，虽然 `subst` 能绕，但别冒险）：
   ```
   subst X: "C:\Users\yyn\Desktop\ESP32-S31  ks\小智ai接入"
   ```
3. 在 **已激活 IDF v6.1 的终端**（不是 Git-Bash，不是普通 PowerShell）里：
   ```
   cd /d X:\
   build.bat
   ```
   `build.bat` 里已经把 `IDF_SKIP_CHECK_SUBMODULES`、`MSYSTEM` 清空、工具链 PATH 都设好了。
   如果仍卡在 `git describe`，就按上表把 `esp-idf\.git` 临时改名。
4. 烧录：
   ```
   cd /d X:\
   idf.py --preview -p COM8 flash monitor
   ```

## 已经做过的静态检查

- 所有 C 文件按 IDF v6.1 的 API 写：`i2s_std` 用 `i2s_new_channel/i2s_channel_init_std_mode/i2s_channel_enable`；
  `esp_http_client` 用 `open/write/fetch_headers/read` 分块上传，避免一次性拼 256KB buffer；
- `audio_out.c` / `ds_tts.c` / `cfg_store.c` / `wifi_prov.c` 四个文件来自本仓库 `音色/` 模块，
  那套代码**已经在 S31 上编译并跑通过**（串口日志见 `音色/build_run*.log`），本工程只改了功放后端与引脚；
- `mic_i2s.c` 来自 `喇叭/复述/`，INMP441 那套同样验证过出数据；
- 新写的只有 `asr_client.c`（multipart 上传）、`llm_client.c`（chat/completions）、`dialog.c`（VAD 与状态机）、
  `main.c`、`app_config.h`，接口签名都对照官方文档核对过。

也就是说：**四个复用模块是已验证的，新增的三个网络/状态机文件需要真机验证**。
