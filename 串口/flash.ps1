# 一键烧录 chuankou 模块（ESP32-S31 / target esp32s31 preview）
#
# 用法：
#   双击 flash.bat                                  自动找端口 + 烧录
#   flash.bat -Port COM7                            指定端口
#   flash.bat -Monitor                              烧完直接看日志（Ctrl+] 退出）
#
# 说明：
#   - 会自动激活 IDF v6.1.0（本机路径），无需你先开 IDF 终端
#   - 烧录走板载 USB 转串口 CH340（VID_1A86 / PID_7523），不是原生 USB
#   - 若 build/ 不存在会先完整编译（首次约 10~20 分钟，本机可能卡死，见 README §11）

param(
    [string]$Port = "",
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

# ---------- 1. 激活 IDF ----------
$idfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (-not (Test-Path $idfProfile)) {
    Write-Host "[X] 找不到 IDF 激活脚本: $idfProfile" -ForegroundColor Red
    exit 1
}
Write-Host "[1/4] 激活 ESP-IDF v6.1.0 ..." -ForegroundColor Cyan
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. $idfProfile | Out-Null

# ---------- 2. 找端口 ----------
if (-not $Port) {
    Write-Host "[2/4] 查找板子串口 ..." -ForegroundColor Cyan
    $cands = @()
    foreach ($d in (Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue)) {
        $m = [regex]::Match($d.FriendlyName, '\((COM\d+)\)')
        if (-not $m.Success) { continue }
        # 只认 USB 串口：CH34x(1A86)、Espressif 原生 USB(303A)
        if ($d.InstanceId -notmatch 'VID_1A86|VID_303A') { continue }
        $pri = 1
        if ($d.InstanceId -match 'PID_7523') { $pri = 0 }   # CH340 优先
        $cands += [pscustomobject]@{ Port = $m.Groups[1].Value; Pri = $pri; Name = $d.FriendlyName }
    }
    if ($cands.Count -eq 0) {
        Write-Host "[X] 没找到任何 USB 串口设备。" -ForegroundColor Red
        Write-Host ""
        Write-Host "  请确认：" -ForegroundColor Yellow
        Write-Host "   1) USB 线插在板子上标 UART 的口（CH340），不是只有供电的口"
        Write-Host "   2) 换一根数据线（很多 Type-C 线只能充电）"
        Write-Host "   3) 设备管理器里应出现「USB-SERIAL CH340 (COMx)」"
        Write-Host "   4) 若显示「未知设备」，装 CH341SER 驱动后重试"
        exit 2
    }
    $cands = $cands | Sort-Object Pri, Port
    $Port = $cands[0].Port
    Write-Host "      选定端口: $Port  ($($cands[0].Name))" -ForegroundColor Green
    if ($cands.Count -gt 1) {
        Write-Host "      其它候选: $(($cands[1..($cands.Count-1)] | ForEach-Object { $_.Port }) -join ', ')"
    }
} else {
    Write-Host "[2/4] 使用指定端口: $Port" -ForegroundColor Cyan
}

# ---------- 3. 需要时先编译 ----------
$bin = Join-Path $PSScriptRoot 'build\kskbl_s31_bridge.bin'
if (-not (Test-Path $bin)) {
    Write-Host "[3/4] 没有固件，先编译（首次较慢）..." -ForegroundColor Cyan
    $env:IDF_PY_BUILD_JOBS = '3'
    if (-not (Test-Path (Join-Path $PSScriptRoot 'sdkconfig'))) {
        idf.py --preview set-target esp32s31
    }
    idf.py --preview build
    if ($LASTEXITCODE -ne 0) { Write-Host "[X] 编译失败" -ForegroundColor Red; exit 3 }
} else {
    Write-Host "[3/4] 已有固件: build\kskbl_s31_bridge.bin" -ForegroundColor Cyan
}

# ---------- 4. 烧录 ----------
Write-Host "[4/4] 烧录到 $Port ..." -ForegroundColor Cyan
idf.py --preview -p $Port flash
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 烧录失败。检查：共地/线材/是否被别的串口工具占用/是否需要按住 BOOT" -ForegroundColor Red
    exit 4
}

Write-Host ""
Write-Host "[OK] 烧录完成" -ForegroundColor Green
Write-Host "     手机 BLE 助手搜 kskbl zdjd -> 写 ON/OFF 验收" -ForegroundColor Green

if ($Monitor) {
    Write-Host ""
    Write-Host "打开串口日志（Ctrl+] 退出）..." -ForegroundColor Cyan
    idf.py --preview -p $Port monitor
}
