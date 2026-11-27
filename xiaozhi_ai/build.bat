@echo off
rem ============================================================
rem  xiaozhi_ai build script (ESP32-S31 / IDF v6.1.0 preview)
rem
rem  NOTE: this file must stay pure ASCII. cmd.exe mis-parses
rem  UTF-8 Chinese comments even after "chcp 65001" when the
rem  script also contains "set" lines, and then the variables
rem  silently fail to be set.
rem
rem  Usage:  build.bat            set-target (first time) + build
rem          build.bat fullclean  wipe build dir
rem          build.bat flash      build + flash on COM8
rem ============================================================
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "MSYSTEM="
set "MSYS="
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PATH=D:\esp\v6.1\esp-idf"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv"
set "ESP_IDF_VERSION=6.1"
rem IDF's git_submodules.cmake runs "git submodule status" and then
rem "git submodule update --init --recursive" when a submodule is
rem missing. On a restricted network that hangs forever right after
rem the "-- Found Git:" line. These projects use no submodules.
set "IDF_SKIP_CHECK_SUBMODULES=1"
set "IDF_PY_BUILD_JOBS=2"
set "PATH=C:\Espressif\tools\git\cmd;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin;C:\Espressif\tools\esp-clangd\esp-21.1.3_20260408\esp-clangd\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.1\venv\Scripts;%PATH%"

set "BD=build_x"

if /i "%~1"=="fullclean" (
    if exist %BD% rmdir /s /q %BD%
    echo [OK] build dir removed
    exit /b 0
)

if not exist %BD% (
    echo [STEP 1/2] set-target esp32s31
    idf.py -B %BD% --preview set-target esp32s31
    if errorlevel 1 goto fail
) else (
    echo [INFO] build dir exists, skip set-target
)

if /i "%~1"=="flash" (
    echo [STEP 2/2] build + flash
    idf.py -B %BD% --preview -p COM8 flash
    if errorlevel 1 goto fail
    echo [OK] flashed
    exit /b 0
)

echo [STEP 2/2] build
idf.py -B %BD% --preview build
if errorlevel 1 goto fail

echo [OK] build finished
exit /b 0

:fail
echo [FAIL] step above returned an error
exit /b 1
