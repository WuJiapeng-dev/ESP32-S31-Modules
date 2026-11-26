# port.ps1 - print the first CH343/CH340/PCM210x COM port, or nothing.
# Kept as a separate file so flash.bat does not have to escape
# PowerShell pipelines (cmd's for/f mangles "^|" and PowerShell then
# receives a literal caret, which fails with "positional parameter").
$ErrorActionPreference = 'SilentlyContinue'
$c = Get-CimInstance Win32_PnPEntity |
     Where-Object { $_.Name -match 'COM\d+' -and $_.Name -match 'CH343|CH340|CP210|USB-SERIAL|USB-Enhanced' } |
     Select-Object -First 1
if ($c) { [regex]::Match($c.Name, 'COM\d+').Value }
