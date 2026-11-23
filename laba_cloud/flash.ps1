# 一键烧录 laba_cloud 模块（ESP32-S31 / target esp32s31 preview）
#
# 用法：
#   双击 flash.bat                                  自动找端口 + 编译 + 烧录
#   flash.bat -Port COM8                            指定端口
#   flash.bat -Monitor                              烧完直接看日志
#
# 说明：
#   - 自动激活 IDF v6.1.0，不用先开 IDF 终端
#   - 烧录口是板载 USB 转串口 CH343（VID_1A86 / PID_55D3）
#   - 本模块没有串口业务占用，UART0 空闲；但 console 仍走 USB Serial/JTAG，
#     所以 COM8 上看不到日志，要看日志请插板子的原生 USB 口

param(
    [string]$Port = "",
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

$idfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (-not (Test-Path $idfProfile)) {
    Write-Host "[X] 找不到 IDF 激活脚本: $idfProfile" -ForegroundColor Red
    exit 1
}
Write-Host "[1/4] 激活 ESP-IDF v6.1.0 ..." -ForegroundColor Cyan
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. $idfProfile | Out-Null

if (-not $Port) {
    Write-Host "[2/4] 查找板子串口 ..." -ForegroundColor Cyan
    $cands = @()
    foreach ($d in (Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue)) {
        $m = [regex]::Match($d.FriendlyName, '\((COM\d+)\)')
        if (-not $m.Success) { continue }
        if ($d.InstanceId -notmatch 'VID_1A86|VID_303A') { continue }
        $pri = 1
        if ($d.InstanceId -match 'PID_55D3') { $pri = 0 }
        $cands += [pscustomobject]@{ Port = $m.Groups[1].Value; Pri = $pri; Name = $d.FriendlyName }
    }
    if ($cands.Count -eq 0) {
        Write-Host "[X] 没找到 USB 串口设备，检查线材与驱动" -ForegroundColor Red
        exit 2
    }
    $cands = $cands | Sort-Object Pri, Port
    $Port = $cands[0].Port
    Write-Host "      选定端口: $Port  ($($cands[0].Name))" -ForegroundColor Green
} else {
    Write-Host "[2/4] 使用指定端口: $Port" -ForegroundColor Cyan
}

$bin = Join-Path $PSScriptRoot 'build\laba_s31_cloud.bin'
if (-not (Test-Path $bin)) {
    Write-Host "[3/4] 没有固件，先编译（首次较慢）..." -ForegroundColor Cyan
    $env:IDF_PY_BUILD_JOBS = '3'
    if (-not (Test-Path (Join-Path $PSScriptRoot 'sdkconfig'))) {
        idf.py --preview set-target esp32s31
    }
    idf.py --preview build
    if ($LASTEXITCODE -ne 0) { Write-Host "[X] 编译失败" -ForegroundColor Red; exit 3 }
} else {
    Write-Host "[3/4] 已有固件: build\laba_s31_cloud.bin" -ForegroundColor Cyan
}

Write-Host "[4/4] 烧录到 $Port ..." -ForegroundColor Cyan
idf.py --preview -p $Port flash
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 烧录失败" -ForegroundColor Red
    exit 4
}

Write-Host ""
Write-Host "[OK] 烧录完成" -ForegroundColor Green
Write-Host "     手机连热点 kskbl-setup -> 浏览器开 http://192.168.4.1/ 配置" -ForegroundColor Green

if ($Monitor) {
    Write-Host ""
    idf.py --preview -p $Port monitor
}
