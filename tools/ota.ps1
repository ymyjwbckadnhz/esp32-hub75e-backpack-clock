<#
.SYNOPSIS
  一键编译 + Arduino OTA 升级（跳过 Arduino IDE 的网络端口发现）

.DESCRIPTION
  IDE 2.x 在 Windows 上有时拿不到网络端口的 port 属性，
  会报 `invalid int value: '{upload.port.properties.port}'`。
  这个脚本直接用 espota 推固件，不依赖 mDNS 发现。

.EXAMPLE
  .\ota.ps1
  .\ota.ps1 -Ip 10.0.0.50
  .\ota.ps1 -BuildOnly
#>

[CmdletBinding()]
param(
  [string]$Ip = '10.0.0.43',
  [int]$Port = 3232,
  [string]$Password = '2018aptx',
  [string]$Fqbn = 'esp32:esp32:esp32:PartitionScheme=min_spiffs',
  [switch]$BuildOnly,
  [switch]$NoReboot
)

$ErrorActionPreference = 'Continue'   # 原生程序的 stderr 提示不应当作异常
$repo = Split-Path $PSScriptRoot -Parent
$sketch = Join-Path $repo 'src\ntp_clock_bmpfont'
$build  = Join-Path $repo '_build'

# ---- 找 arduino-cli ----
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
if (-not (Test-Path $cli)) {
  $cli = (Get-Command arduino-cli -EA SilentlyContinue).Source
}
if (-not $cli) { throw '找不到 arduino-cli' }

# ---- 找 espota ----
$espota = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32" -Recurse -Filter 'espota.exe' -EA SilentlyContinue |
          Select-Object -First 1 -ExpandProperty FullName
if (-not $espota) { throw '找不到 espota.exe（esp32 核心未安装？）' }

Write-Host "==> 编译 ($Fqbn)" -ForegroundColor Cyan
$compileOut = & $cli compile --fqbn $Fqbn --build-path $build $sketch 2>&1 | Out-String
$rc = $LASTEXITCODE
($compileOut -split "`n" | Select-String -Pattern 'Sketch uses|error:') | ForEach-Object { Write-Host ("    " + $_.Line.Trim()) }
if ($rc -ne 0) { throw "编译失败 (exit $rc)" }

$bin = Join-Path $build 'ntp_clock_bmpfont.ino.bin'
if (-not (Test-Path $bin)) { throw "没生成固件: $bin" }
Write-Host ("    固件 {0:N0} KB" -f ((Get-Item $bin).Length / 1KB))

if ($BuildOnly) { Write-Host '仅编译，未上传'; return }

Write-Host "==> OTA 上传到 $Ip`:$Port" -ForegroundColor Cyan
# 用完整参数数组，避免单元素数组被解包成字符串后按字符拆开
$espArgs = @('-i', $Ip, '-p', "$Port", '-a', $Password, '-f', $bin)
if (-not $NoReboot) { $espArgs += '-r' }
$out = & $espota @espArgs 2>&1 | Out-String

if ($out -match '100% Done') {
  Write-Host '==> 成功' -ForegroundColor Green
} else {
  ($out -split "`n" | Select-String 'ERROR|WARNING|No response' | Select-Object -First 3) |
    ForEach-Object { Write-Host $_.Line -ForegroundColor Yellow }
  throw 'OTA 失败。常见原因：设备正在拉数据（阻塞主循环）→ 重试；或 IP/密码不对'
}

# ---- 等重启并验证 ----
if (-not $NoReboot) {
  Write-Host '==> 等待设备重启…' -ForegroundColor Cyan
  $ok = $false
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 2
    try {
      $wc = New-Object System.Net.WebClient
      $wc.Proxy = $null
      $s = $wc.DownloadString("http://$Ip/api/status")
      Write-Host "    在线: $s" -ForegroundColor Green
      $ok = $true
      break
    } catch { }
  }
  if (-not $ok) { Write-Host '    设备未在 40 秒内恢复，请检查' -ForegroundColor Yellow }
}
