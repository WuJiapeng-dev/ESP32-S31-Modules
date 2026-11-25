@echo off
rem ============================================================
rem  Expose the ESP32 console to the internet through a tunnel.
rem  The board keeps serving plain HTTP; cloudflared terminates
rem  TLS and forwards to it, so the browser sees one https URL.
rem
rem  Usage: double-click, or run:  public-link.bat 192.168.4.1
rem ============================================================
setlocal
cd /d "%~dp0"

set "BOARD=%~1"
if "%BOARD%"=="" set "BOARD=192.168.4.1"
set "URL=http://%BOARD%"

echo.
echo   Target board : %URL%
echo.

if not exist "cloudflared.exe" (
  echo   [cloudflared.exe not found in this folder]
  echo.
  echo   Download it once, save it next to this .bat:
  echo   https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe
  echo.
  echo   Then rename to: cloudflared.exe
  echo.
  pause
  exit /b 1
)

echo   Checking the board is reachable ...
powershell -NoProfile -Command "try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 6 '%URL%/api/state'; Write-Host ('   board OK, state: ' + $r.Content) } catch { Write-Host '   cannot reach the board - connect this PC to the board Wi-Fi first'; exit 1 }"
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)

echo.
echo   Starting tunnel. Look for the https://xxxx.trycloudflare.com line below.
echo   Keep this window open; closing it takes the public URL offline.
echo.
cloudflared.exe tunnel --url %URL% --no-autoupdate

echo.
echo   Tunnel closed.
pause
