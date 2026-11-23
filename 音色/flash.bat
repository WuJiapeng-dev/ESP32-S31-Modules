@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ============================================
echo  yinse 模块一键烧录 (ESP32-S31)
echo ============================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (echo 完成。) else (echo 失败，退出码 %RC%，请看上方提示。)
pause
