#
# HmRdp - 把**已签名**的 HAP 部署到真机（或模拟器）并启动。
#
# 为什么不需要在这里签名（重要，别再加回来）：
#   * 签名由 **hvigor** 在 `devecocli build` 阶段完成。工程级 `build-profile.json5` 的
#     `signingConfigs` 恒为 `[]`（不提交任何签名信息）；本机签名配置放在仓库外的
#     `.signing/signing-config.json`，由 `hvigorfile.ts` 通过
#     `config.ohos.overrides.signingConfig` 注入（官方《动态修改编译配置》方式）。
#   * DevEco「自动签名」写进 build-profile.json5 的 storePassword/keyPassword 是**密文**，
#     只有 DevEco/hvigor 能解；外部工具（hap-sign-tool）需要**明文**口令，因此**不在此处签名**。
#   * 于是仓库里永远没有签名路径/口令，别人 clone 后只是构建出未签名 HAP。
#
# 产物：entry/build/<product>/outputs/<product>/entry-<product>-signed.hap
#       （没有签名配置时退化为 -unsigned.hap，模拟器可直接安装）
#
# 用法：
#   devecocli build                                     # 真机用默认 product=default（arm64）
#   native/scripts/install-device.ps1 -Device "<真机序列号>"
#   native/scripts/install-device.ps1 -Device "<序列号>" -Uninstall -NoLaunch
#   不传 -Device 时：自动挑选**唯一**的物理设备（排除 127.0.0.1:* / emulator-*）。
#
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
