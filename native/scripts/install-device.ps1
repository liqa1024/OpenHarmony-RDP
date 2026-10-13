# 把已签名 HAP 部署到真机并启动。签名由 hvigor 在 build 阶段完成（配置在仓库外
# .signing/，经 hvigorfile.ts 注入），本脚本只安装启动、不接触签名材料。
param(
  [string]$Device = "",
  [string]$Product = "default",
  [switch]$Uninstall,
  [switch]$NoLaunch
)
$ErrorActionPreference = "Stop"

$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$OutDir = Join-Path $Repo "entry\build\$Product\outputs\$Product"
$Signed = Join-Path $OutDir "entry-$Product-signed.hap"
$Unsigned = Join-Path $OutDir "entry-$Product-unsigned.hap"
$Hap = ""
if (Test-Path $Signed) { $Hap = $Signed } elseif (Test-Path $Unsigned) { $Hap = $Unsigned }
if ([string]::IsNullOrWhiteSpace($Hap)) {
  Write-Host "install-device: 找不到产物（$Signed / $Unsigned）" -ForegroundColor Red
  Write-Host "                先运行 'devecocli build'（真机用 product=default）" -ForegroundColor Red
  exit 1
}
if ($Hap -eq $Unsigned) {
  Write-Host "install-device: 注意：只有未签名 HAP（本机没有 .signing/signing-config.json 或未生效）" -ForegroundColor Yellow
}

$Hdc = (Get-Command hdc -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($Hdc)) { Write-Host "install-device: 找不到 hdc" -ForegroundColor Red; exit 1 }

$serial = $Device
if ([string]::IsNullOrWhiteSpace($serial)) {
  $targets = @(& $Hdc list targets 2>$null | Where-Object { $_ -match "\S" -and $_ -notmatch "\[Empty\]" })
  $physical = @($targets | Where-Object { $_ -notmatch "^127\.0\.0\.1:" -and $_ -notmatch "^emulator-" })
  if ($physical.Count -eq 1) {
    $serial = ($physical[0] -split "\s+")[0]
  } elseif ($physical.Count -eq 0) {
    Write-Host "install-device: 没有物理设备。用 -Device 指定，或确认 hdc 已连接。targets: $($targets -join ', ')" -ForegroundColor Red
    exit 1
  } else {
    Write-Host "install-device: 多台物理设备，请用 -Device 指定：$($physical -join ', ')" -ForegroundColor Red
    exit 1
  }
}

Write-Host "install-device: $Hap -> $serial" -ForegroundColor Cyan
if ($Uninstall) { & $Hdc -t $serial uninstall com.lixa.hmrdp 2>&1 | Out-Null }
$out = & $Hdc -t $serial install -r "$Hap" 2>&1
Write-Host ($out -join "`n")
if (($out -join "`n") -notmatch "success") { Write-Host "install-device: 安装失败" -ForegroundColor Red; exit 1 }

if (-not $NoLaunch) {
  & $Hdc -t $serial shell "aa start -a EntryAbility -b com.lixa.hmrdp" 2>&1 | Out-Null
  Write-Host "install-device: 已启动。读日志：hdc -t $serial shell \`"hilog -x -D 0xD001\`"" -ForegroundColor Green
}
