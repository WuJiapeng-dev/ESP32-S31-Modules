@echo off
rem ============================================================
rem  phone_recv build script  (ESP32-S31 / IDF v6.1.0 preview target)
rem
rem  Why cmd instead of PowerShell:
rem  PS 5.1 re-encodes non-ASCII path args to the ANSI codepage
rem  when calling the idf.py wrapper, so a Chinese folder name
rem  arrives at CMake as garbage. cmd + %~dp0 keeps the command
rem  line pure ASCII and the path intact.
rem
rem  Even so, this project must live in a pure-ASCII path:
rem  ESP-IDF's gcc turns non-ASCII spec paths into \x0a and dies
rem  with "cannot read spec file". Build from C:\espwork\phone_recv.
rem
rem  Usage:  build.bat             build all
rem          build.bat bootloader  bootloader only
rem          build.bat fullclean   clean
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
set "PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin;C:\Espressif\tools\esp-clangd\esp-21.1.3_20260408\esp-clangd\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.1\venv\Scripts;%PATH%"
set "IDF_PY_BUILD_JOBS=2"

set "BD=build_check"

if "%~1"=="" (
    echo [1/2] bootloader ...
    idf.py -B %BD% --preview bootloader
    if errorlevel 1 goto :fail
    echo [2/2] main project ...
    idf.py -B %BD% --preview build
) else (
    idf.py -B %BD% --preview %*
)
if errorlevel 1 goto :fail
echo.
echo ===== DONE =====
if exist "%BD%\phone_recv.bin" for %%F in ("%BD%\phone_recv.bin") do echo BIN: %BD%\phone_recv.bin  (%%~zF bytes)
goto :eof

:fail
echo.
echo ===== FAILED, exit code %ERRORLEVEL% =====
exit /b 1
