# Regenerates the frozen `perl Configure` output native/openssl-cmake builds from
# (export-snapshot.pl); only needed when the pinned OpenSSL version or options change.
param(
  [string[]]$Arch = @("arm64-v8a", "x86_64"),
  [switch]$Configure
)
$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Out = "$Native\openssl-cmake"

function ConvertTo-Wsl([string]$Path) {
  $p = (& wsl wslpath -a $Path).Trim()
  if ($LASTEXITCODE -ne 0) { throw "wslpath failed for $Path" }
  return $p
}

foreach ($a in $Arch) {
  $BuildTree = "$Native\build\openssl-$a"
  $Configured = "$BuildTree\openssl"

  if ($Configure) {
    $WslScript = ConvertTo-Wsl "$PSScriptRoot\build-openssl-wsl.sh"
    $WslSrc = ConvertTo-Wsl "$Native\third_party\openssl-3.0.15"
    $WslBuild = ConvertTo-Wsl $BuildTree
    $WslPrefix = ConvertTo-Wsl "$Native\install\$a\openssl"
    Write-Host "=== configuring OpenSSL ($a) in WSL ==="
    & wsl -e bash $WslScript $WslSrc $WslBuild $WslPrefix $a
    if ($LASTEXITCODE -ne 0) { throw "build-openssl-wsl.sh failed for $a" }
  }

  if (-not (Test-Path -LiteralPath "$Configured\configdata.pm")) {
    throw "no configured tree at $Configured`nRun build-openssl-wsl.sh for $a, or pass -Configure."
  }

  $WslConfigured = ConvertTo-Wsl $Configured
  $WslOut = ConvertTo-Wsl $Out
  $WslScript = ConvertTo-Wsl "$Out\export-snapshot.pl"
  Write-Host "=== exporting snapshot ($a) ==="
  & wsl -e perl $WslScript $WslConfigured $WslOut $a
  if ($LASTEXITCODE -ne 0) { throw "export-snapshot.pl failed for $a" }
}

Write-Host "=== snapshot refreshed under $Out\generated (commit it) ==="
