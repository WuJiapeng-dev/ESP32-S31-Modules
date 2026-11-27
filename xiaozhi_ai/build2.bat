@echo off
rem ============================================================
rem  xz_ai - direct CMake/Ninja build (bypass idf.py python layer)
rem
rem  Three environment problems on this machine, all worked around:
rem   1) idf.py stalls at "idf_tools.py check-python-dependencies"
rem      -> drive cmake + ninja directly instead of idf.py.
rem   2) IDF's own .git has a submodule whose `git status` hangs
rem      -> D:\esp\v6.1\esp-idf\.git was renamed to .git_disabled.
rem   3) idf_component_manager blocks on a network call (CPU 0:00,
rem      stuck for 20+ min) -> IDF_COMPONENT_MANAGER=0. This project
rem      uses no managed components, so the manager is not needed.
rem
rem  Note: every process spawn on this box costs 10-40 s (security
rem  software scanning each new .exe), so a full build takes a while.
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
set "IDF_SKIP_CHECK_SUBMODULES=1"
set "IDF_COMPONENT_MANAGER=0"
set "PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin;C:\Espressif\tools\esp-clangd\esp-21.1.3_20260408\esp-clangd\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.1\venv\Scripts;%PATH%"

set "BD=build3"

if not exist "%BD%\CMakeCache.txt" (
    echo [CMAKE] configure
    cmake -G Ninja -B %BD% ^
      -DIDF_TARGET=esp32s31 ^
      -DCMAKE_TOOLCHAIN_FILE="%IDF_PATH%\tools\cmake\toolchain-esp32s31.cmake" ^
      -DSDKCONFIG="%BD%\sdkconfig" ^
      -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" ^
      -DPYTHON_DEPS_CHECKED=1 ^
      -DCCACHE_ENABLE=0 ^
      .
    if errorlevel 1 goto fail
)

echo [NINJA] build
ninja -C %BD% -j 6
if errorlevel 1 goto fail

echo.
echo ===== DONE =====
if exist "%BD%\xiaozhi_ai.bin" for %%F in ("%BD%\xiaozhi_ai.bin") do echo BIN: %BD%\xiaozhi_ai.bin  (%%~zF bytes)
exit /b 0

:fail
echo.
echo ===== FAILED =====
exit /b 1
