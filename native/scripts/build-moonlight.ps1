# Cross-compiles moonlight-common-c (plus its bundled enet and nanors) for OHOS
# and collects the static archives the app links.
#
# moonlight-common-c is built as *static* archives, not a shared library: unlike
# FreeRDP it has no plugin loader that needs symbols at runtime, so there is
# nothing to package into entry/libs/<abi>/ - the archives are linked straight
# into libhmrdp.so. OpenSSL's libcrypto is the only external dependency (the
# library uses it for the pairing AES), and it is the same static build FreeRDP
# already uses.
#
#   native/scripts/build-moonlight.ps1 [-Arch arm64-v8a] [-Clean]
#
# Prerequisites: fetch-sources.ps1 (patched tree) and build-openssl-wsl.sh
# (native/install/<arch>/openssl/lib/libcrypto.a).
param(
  [string]$Arch = "arm64-v8a",
  [switch]$Clean
)
$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Deveco = if ($env:DEVECO_HOME) { $env:DEVECO_HOME } else { "C:\Program Files\Huawei\DevEco Studio" }
$Ndk = "$Deveco\sdk\default\openharmony\native"
$Toolchain = "$Ndk\build\cmake\ohos.toolchain.cmake"
$Ninja = (Get-Command ninja).Source

$Src = "$Native\third_party\moonlight-common-c"
$Build = "$Native\build\moonlight-$Arch"
$Out = "$Native\install\$Arch\moonlight"
$OpenSsl = "$Native\install\$Arch\openssl"

if (-not (Test-Path -LiteralPath "$Src\CMakeLists.txt")) {
  throw "moonlight-common-c source not found: $Src`nRun native/scripts/fetch-sources.ps1 first."
}
if (-not (Test-Path -LiteralPath "$OpenSsl\lib\libcrypto.a")) {
  throw "OpenSSL static libcrypto not found: $OpenSsl\lib\libcrypto.a`nBuild OpenSSL first (native/scripts/build-openssl-wsl.sh -Arch $Arch)."
}

if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
if ($Clean -and (Test-Path $Out)) { Remove-Item -Recurse -Force $Out }

$cfg = @(
  "-S", $Src, "-B", $Build, "-G", "Ninja",
  "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
  "-DCMAKE_MAKE_PROGRAM=$Ninja",
  "-DOHOS_ARCH=$Arch",
  "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
  # Both the library and enet are PIC so the archives can go inside libhmrdp.so.
  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
  "-DBUILD_SHARED_LIBS=OFF",
  # find_package(OpenSSL 1.0.2 REQUIRED) needs the static crypto FreeRDP already
  # uses; the paths bypass a system OpenSSL (there is none for OHOS).
  "-DOPENSSL_ROOT_DIR=$OpenSsl",
  "-DOPENSSL_INCLUDE_DIR=$OpenSsl\include",
  "-DOPENSSL_CRYPTO_LIBRARY=$OpenSsl\lib\libcrypto.a",
  "-DOPENSSL_SSL_LIBRARY=$OpenSsl\lib\libssl.a",
  "-DOPENSSL_USE_STATIC_LIBS=ON"
)

& cmake @cfg
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& cmake --build $Build --parallel 8
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

# The upstream CMakeLists has no install() rules (cmake --install would produce
# nothing), so collect the two archives and the public header by hand.
New-Item -ItemType Directory -Path "$Out\lib" -Force | Out-Null
New-Item -ItemType Directory -Path "$Out\include" -Force | Out-Null
Copy-Item -LiteralPath "$Build\libmoonlight-common-c.a" -Destination "$Out\lib\" -Force
Copy-Item -LiteralPath "$Build\enet\libenet.a" -Destination "$Out\lib\" -Force
Copy-Item -LiteralPath "$Src\src\Limelight.h" -Destination "$Out\include\" -Force

Write-Host "=== moonlight-common-c ($Arch) collected in $Out ==="
Get-ChildItem "$Out\lib" | Select-Object Name, Length

# Keep entry/src/main/cpp/thirdparty/ in sync (generated, gitignored): the
# moonlight archives plus the OpenSSL headers/libssl/libcrypto the module links
# once for both the client and the HTTP/pairing layer.
& (Join-Path $PSScriptRoot "sync-moonlight.ps1") -Arch $Arch
if ($LASTEXITCODE -ne 0) { throw "sync-moonlight.ps1 failed" }
& (Join-Path $PSScriptRoot "sync-openssl.ps1") -Arch $Arch
if ($LASTEXITCODE -ne 0) { throw "sync-openssl.ps1 failed" }
