@echo off
rem ============================================================
rem  maikefeng flash script  (ESP32-S31 / prebuilt binaries)
rem
rem  This file is intentionally pure ASCII: cmd.exe parses the
rem  batch in the console code page (GBK on this machine), so a
rem  UTF-8 Chinese batch gets shredded into broken commands.
rem
rem  Usage:
rem    flash.bat              auto-detect CH343/CH340 port and flash
rem    flash.bat COM8         use the given port
rem    flash.bat COM8 check   only read chip id, do not write
rem
rem  Why not "idf.py flash": it runs ninja first, and ninja/gcc
rem  dies on non-ASCII project paths. This script calls esptool
rem  directly and only reads build_check\flash_args, so it works
rem  from any directory and does not recompile anything.
rem ============================================================
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "BD=build_check"
set "PY=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
set "PORT=%~1"
set "MODE=%~2"

rem ---------- 1. check artifacts ----------
if not exist "%BD%\flash_args" (
    echo [X] %BD%\flash_args not found - run build.bat first
    pause
    exit /b 1
)
if not exist "%BD%\maikefeng.bin" (
    echo [X] %BD%\maikefeng.bin not found - run build.bat first
    pause
    exit /b 1
)
if not exist "%PY%" (
    echo [X] python not found: %PY%
    echo     check IDF v6.1.0 install path
    pause
    exit /b 1
)

rem ---------- 2. find port ----------
if "%PORT%"=="" (
    for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "$c=Get-CimInstance Win32_PnPEntity ^| Where-Object { $_.Name -match 'COM\d+' -and $_.Name -match 'CH343^|CH340^|CP210^|USB-SERIAL^|USB-Enhanced' } ^| Select-Object -First 1; if ($c) { [regex]::Match($c.Name,'COM\d+').Value }"`) do set "PORT=%%P"
)
if "%PORT%"=="" (
    echo [X] no CH343/CH340 port found.
    echo     1^) is the USB cable in the UART port of the board?
    echo     2^) does Device Manager show USB-Enhanced-SERIAL CH343?
    echo     3^) or pass it manually: flash.bat COM8
    pause
    exit /b 1
)

echo ============================================
echo   flash maikefeng to %PORT%
echo   images: bootloader 0x2000 / partition 0x8000 / app 0x70000
echo ============================================
for %%F in ("%BD%\maikefeng.bin") do echo   app size: %%~zF bytes
echo.

rem ---------- 3. flash ----------
if /i "%MODE%"=="check" (
    "%PY%" -m esptool --chip esp32s31 -p %PORT% flash-id
) else (
    pushd "%BD%"
    "%PY%" -m esptool --chip esp32s31 -p %PORT% -b 460800 --before default-reset --after hard-reset --no-stub write-flash "@flash_args"
    set "RC=!ERRORLEVEL!"
    popd
    if not "!RC!"=="0" (
        echo.
        echo [X] flash failed, exit code !RC!
        echo     usual cause: port busy ^(serial monitor / VS Code monitor^)
        pause
        exit /b 1
    )
)

echo.
echo ===== done =====
echo after reset:
echo   1^) connect BLE "kskbl zdjd" from the phone
echo   2^) write PING to 6a400002 -^> expect PONG
echo   3^) write EN:hello-ish label, wait for prompt, say that phrase
echo   4^) write ASR, say the phrase -^> expect ASR:label and playback
echo logs: use the board's native USB port, not the CH343 one
echo.
pause
