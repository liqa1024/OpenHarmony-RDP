# Builds every pinned OHOS native dependency (zlib/openssl/opus/freerdp/moonlight)
# with the Windows OHOS NDK and syncs what entry/ consumes; run fetch-sources.ps1 first.
param(
  [string]$Arch = "arm64-v8a",
  [switch]$Clean,
  [ValidateSet("zlib", "openssl", "opus", "freerdp", "moonlight")]
  [string[]]$Only = @("zlib", "openssl", "opus", "freerdp", "moonlight")
)
$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Repo = Split-Path -Parent $Native
$Deveco = if ($env:DEVECO_HOME) { $env:DEVECO_HOME } else { "C:\Program Files\Huawei\DevEco Studio" }
$Ndk = "$Deveco\sdk\default\openharmony\native"
$Toolchain = "$Ndk\build\cmake\ohos.toolchain.cmake"
$Ninja = (Get-Command ninja).Source
$Third = "$Native\third_party"
$Install = "$Native\install\$Arch"
$BuildRoot = "$Native\build"
$Thirdparty = "$Repo\entry\src\main\cpp\thirdparty"
$Libs = "$Repo\entry\libs\$Arch"

switch ($Arch) {
  "arm64-v8a"   { }
  "armeabi-v7a" { }
  "x86_64"      { }
  default       { throw "unsupported arch: $Arch" }
}
$Triple = switch ($Arch) {
  "arm64-v8a"   { "aarch64-linux-ohos" }
  "armeabi-v7a" { "arm-linux-ohos" }
  "x86_64"      { "x86_64-linux-ohos" }
}

function Assert-Found([string]$Path, [string]$What) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "$What not found: $Path" }
}

function Invoke-CMake {
  param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Src,
    [Parameter(Mandatory)][string]$Build,
    [string]$Prefix,
    [string[]]$Options = @()
  )
  if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
  if ($Clean -and $Prefix -and (Test-Path $Prefix)) { Remove-Item -Recurse -Force $Prefix }

  $cfg = @(
    "-S", $Src, "-B", $Build, "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DOHOS_ARCH=$Arch",
    # RelWithDebInfo: -g keeps native stack frames symbolicable; -O2 is pinned per project.
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
  )
  if ($Prefix) { $cfg += "-DCMAKE_INSTALL_PREFIX=$Prefix" }
  $cfg += $Options

  & cmake @cfg
  if ($LASTEXITCODE -ne 0) { throw "$Name`: cmake configure failed" }
  & cmake --build $Build --parallel 8
  if ($LASTEXITCODE -ne 0) { throw "$Name`: build failed" }
  if ($Prefix) {
    & cmake --install $Build
    if ($LASTEXITCODE -ne 0) { throw "$Name`: install failed" }
  }
}

# Mirror a dir, then bump mtimes: the module build tracks header changes by mtime.
function Copy-Tree([string]$From, [string]$To) {
  if (Test-Path -LiteralPath $To) { Remove-Item -Recurse -Force -LiteralPath $To }
  New-Item -ItemType Directory -Path $To -Force | Out-Null
  Copy-Item -Path (Join-Path $From "*") -Destination $To -Recurse -Force
  $now = Get-Date
  Get-ChildItem -LiteralPath $To -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
}

function Replace-Line([string]$Path, [string]$Pattern, [string]$Replacement) {
  if (-not (Test-Path -LiteralPath $Path)) { return }
  $raw = [System.IO.File]::ReadAllText($Path)
  $new = $raw -replace $Pattern, $Replacement
  if ($new -ne $raw) { [System.IO.File]::WriteAllText($Path, $new) }
}

# Ships one shared library to entry/libs/<abi>/ by name so stale files are replaced.
function Copy-PrebuiltLib([string]$From) {
  if (-not (Test-Path -LiteralPath $From)) { throw "shared library not found: $From" }
  New-Item -ItemType Directory -Path $Libs -Force | Out-Null
  Copy-Item -LiteralPath $From -Destination $Libs -Force
  Write-Host "  -> $(Split-Path -Leaf $From)"
}

function Build-Zlib {
  $src = "$Third\zlib-1.3.1"
  Assert-Found "$src\CMakeLists.txt" "zlib source (run fetch-sources.ps1)"
  Invoke-CMake -Name zlib -Src $src -Build "$BuildRoot\zlib-$Arch" -Prefix "$Install\zlib" -Options @(
    # Shared libz.so in entry/libs/<abi>/; FreeRDP records it in DT_NEEDED.
    "-DBUILD_SHARED_LIBS=ON",
    "-DZLIB_BUILD_EXAMPLES=OFF"
  )
  Copy-PrebuiltLib "$Install\zlib\lib\libz.so"
  Write-Host "=== zlib ($Arch) installed to $Install\zlib ==="
}

function Build-OpenSsl {
  $src = "$Native\openssl-cmake"
  Assert-Found "$Third\openssl-3.0.15\Configure" "OpenSSL source (run fetch-sources.ps1)"
  Assert-Found "$src\generated\sources.cmake" "OpenSSL snapshot (run export-openssl-snapshot.ps1)"

  Invoke-CMake -Name openssl -Src $src -Build "$BuildRoot\openssl-cmake-$Arch" -Prefix "$Install\openssl" -Options @(
    "-DOSSL_SRC=$Third\openssl-3.0.15"
  )
  Sync-OpenSsl
}

# libcrypto.so / libssl.so are the single OpenSSL in the process (DT_NEEDED by
# libwinpr3/libfreerdp3/libmoonlight-common-c); no headers are synced.
function Sync-OpenSsl {
  $from = "$Install\openssl\lib"
  Assert-Found "$from\libcrypto.so" "OpenSSL install (build openssl first)"
  Assert-Found "$from\libssl.so" "OpenSSL install (build openssl first)"

  Copy-PrebuiltLib "$from\libcrypto.so"
  Copy-PrebuiltLib "$from\libssl.so"

  $stale = "$Thirdparty\openssl"
  if (Test-Path -LiteralPath $stale) { Remove-Item -Recurse -Force -LiteralPath $stale }

  Write-Host "=== OpenSSL shared libraries copied to $Libs ==="
}

function Build-Opus {
  $src = "$Third\opus-1.5.2"
  Assert-Found "$src\CMakeLists.txt" "opus source (run fetch-sources.ps1)"
  Invoke-CMake -Name opus -Src $src -Build "$BuildRoot\opus-$Arch" -Prefix "$Install\opus" -Options @(
    # Shared libopus.so in entry/libs/<abi>/ for the Moonlight audio decode;
    # patch-opus.ps1 makes its SONAME version-less.
    "-DBUILD_SHARED_LIBS=ON",
    "-DOPUS_BUILD_SHARED_LIBRARY=ON",
    "-DOPUS_BUILD_PROGRAMS=OFF",
    "-DOPUS_BUILD_TESTING=OFF"
  )
  Copy-PrebuiltLib "$Install\opus\lib\libopus.so"
  Sync-Opus
}

function Sync-Opus {
  $from = "$Install\opus\include\opus"
  $to = "$Thirdparty\opus\include\opus"
  Assert-Found "$from\opus_multistream.h" "opus install headers (build opus first)"

  New-Item -ItemType Directory -Path $to -Force | Out-Null
  Copy-Item -Path "$from\*" -Destination $to -Recurse -Force

  $now = Get-Date
  Get-ChildItem -LiteralPath $to -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
  Write-Host "=== opus headers synced to $Thirdparty\opus\include\opus ==="
}

function Build-FreeRdp {
  $src = "$Third\FreeRDP"
  Assert-Found "$src\CMakeLists.txt" "FreeRDP source (run fetch-sources.ps1)"
  Assert-Found "$Install\openssl\lib\libcrypto.so" "OpenSSL (build openssl first)"
  $zlib = "$Install\zlib"
  $prefix = "$Install\freerdp"

  Invoke-CMake -Name freerdp -Src $src -Build "$BuildRoot\freerdp-$Arch" -Prefix $prefix -Options @(
    "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
    "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    "-DCMAKE_CXX_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    # WINPR_ASSERT must never abort the host app (default ON calls abort() on any
    # internal sanity check); OFF makes it a plain assert(), disabled by NDEBUG.
    "-DWITH_VERBOSE_WINPR_ASSERT=OFF",
    "-DZLIB_LIBRARY=$zlib\lib\libz.so",
    "-DZLIB_INCLUDE_DIR=$zlib\include",
    # OpenSSL/zlib are the shared libs above; FreeRDP records them in DT_NEEDED
    # instead of folding private copies into libwinpr3/libfreerdp3.
    "-DOPENSSL_ROOT_DIR=$Install\openssl",
    "-DOPENSSL_INCLUDE_DIR=$Install\openssl\include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Install\openssl\lib\libcrypto.so",
    "-DOPENSSL_SSL_LIBRARY=$Install\openssl\lib\libssl.so",
    "-DOPENSSL_USE_STATIC_LIBS=OFF",
    "-DWITH_SERVER=OFF", "-DWITH_SAMPLE=OFF",
    "-DWITH_CLIENT=ON", "-DWITH_CLIENT_COMMON=ON", "-DWITH_CLIENT_INTERFACE=OFF",
    "-DWITH_PROXY=OFF", "-DWITH_SHADOW=OFF",
    "-DWITH_CUPS=OFF", "-DWITH_PULSE=OFF", "-DWITH_ALSA=OFF", "-DWITH_OSS=OFF",
    "-DWITH_FFMPEG=OFF", "-DWITH_SWSCALE=OFF",
    "-DWITH_X11=OFF", "-DWITH_WAYLAND=OFF",
    "-DWITH_GSTREAMER_0_10=OFF", "-DWITH_GSTREAMER_1_0=OFF",
    "-DWITH_LIBSYSTEMD=OFF", "-DWITH_PCSC=OFF", "-DWITH_JPEG=OFF",
    "-DWITH_OPENSLES=ON",
    "-DOpenSLES_INCLUDE_DIR=$Ndk\sysroot\usr\include",
    "-DOpenSLES_LIBRARY=$Ndk\sysroot\usr\lib\$Triple\libOpenSLES.so",
    "-DWITH_OPENH264=OFF",
    "-DWITH_GSM=OFF", "-DWITH_LAME=OFF", "-DWITH_FAAD2=OFF", "-DWITH_FAAC=OFF",
    "-DWITH_SOXR=OFF", "-DWITH_OPUS=OFF", "-DWITH_PKCS11=OFF", "-DWITH_ICU=OFF", "-DWITH_KRB5=OFF",
    "-DWITH_UNICODE_BUILTIN=ON", "-DWITH_INTERNAL_RC4=ON", "-DWITH_INTERNAL_MD4=ON", "-DWITH_INTERNAL_MD5=ON",
    "-DWITH_FUSE=OFF", "-DWITH_CLIENT_SDL=OFF", "-DWITH_MANPAGES=OFF",
    "-DBUILD_SHARED_LIBS=ON",
    "-DWITH_CHANNELS=ON", "-DWITH_CLIENT_CHANNELS=ON", "-DBUILTIN_CHANNELS=ON",
    "-DWITH_CAIRO=OFF", "-DWITH_SDL_IMAGE_DIALOGS=OFF", "-DWITH_WEBVIEW=OFF",
    "-DWITH_PLATFORM_SERVER=OFF", "-DWITH_PROGRESS_BAR=OFF",
    # WITH_SIMD=ON brings FreeRDP's SSE/NEON primitives (inverse DWT, YCbCr->RGB,
    # YCoCg); not bit-identical to generic C, so correctness is measured (rdp-gfx.md).
    "-DWITH_SIMD=ON", "-DWITH_AAD=OFF", "-DWITH_SMARTCARD=OFF",
    "-DWITH_KEYBOARD_LAYOUT_FROM_FILE=OFF",
    "-DCHANNEL_AUDIN=ON", "-DCHANNEL_ENCOMSP=OFF", "-DCHANNEL_RAIL=OFF", "-DCHANNEL_REMDESK=OFF",
    "-DCHANNEL_TELEMETRY=OFF", "-DCHANNEL_URBDRC=OFF", "-DCHANNEL_SMARTCARD=OFF",
    "-DCHANNEL_CLIPRDR=ON", "-DCHANNEL_RDPDR=ON", "-DCHANNEL_RDPSND=ON", "-DCHANNEL_RDPSND_CLIENT=ON",
    "-DCHANNEL_DRDYNVC=ON", "-DCHANNEL_DISP=ON", "-DCHANNEL_RDPGFX=ON", "-DCHANNEL_RDPEI=ON",
    "-DCHANNEL_GEOMETRY=OFF", "-DCHANNEL_VIDEO=OFF",
    "-DWITH_WINPR_TOOLS=OFF", "-DWITH_BINARY_VERSIONING=OFF", "-DCMAKE_SKIP_INSTALL_RPATH=ON",
    # One version-less libX.so (SONAME libX.so): AddTargetWithResourceFile.cmake
    # keeps the "lib" prefix and sets the SONAME on non-Windows.
    "-DWITH_LIBRARY_VERSIONING=OFF",
    "-DWITH_ABSOLUTE_PLUGIN_LOAD_PATHS=OFF",
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"
  )

  Sync-FreeRdp
  Copy-FreeRdpLibs
}

function Sync-FreeRdp {
  $from = "$Install\freerdp\include"
  $to = "$Thirdparty\freerdp\include"
  Assert-Found $from "FreeRDP install headers (build freerdp first)"

  Copy-Tree $from $to
  # Strip build-machine paths / git revision from the generated headers.
  Replace-Line (Join-Path $to "winpr3\winpr\build-config.h") `
    '(?m)^#define WINPR_INSTALL_PREFIX .*$' '#define WINPR_INSTALL_PREFIX "freerdp"'
  Replace-Line (Join-Path $to "winpr3\winpr\build-config.h") `
    '(?m)^#define WINPR_INSTALL_SYSCONFDIR .*$' '#define WINPR_INSTALL_SYSCONFDIR "freerdp/etc"'
  Replace-Line (Join-Path $to "freerdp3\freerdp\version.h") `
    '(?m)^#define FREERDP_GIT_REVISION .*$' '#define FREERDP_GIT_REVISION "n/a"'
  Replace-Line (Join-Path $to "winpr3\winpr\version.h") `
    '(?m)^#define WINPR_GIT_REVISION .*$' '#define WINPR_GIT_REVISION "n/a"'
  Write-Host "=== FreeRDP headers synced to $to ==="
}

function Copy-FreeRdpLibs {
  Get-ChildItem -LiteralPath "$Install\freerdp\lib" -Filter *.so | ForEach-Object {
    Copy-PrebuiltLib $_.FullName
  }
  Write-Host "=== FreeRDP shared libraries copied to $Libs ==="
}

function Build-Moonlight {
  $src = "$Third\moonlight-common-c"
  Assert-Found "$src\CMakeLists.txt" "moonlight-common-c source (run fetch-sources.ps1)"
  Assert-Found "$Install\openssl\lib\libcrypto.so" "OpenSSL (build openssl first)"
  $build = "$BuildRoot\moonlight-$Arch"
  $out = "$Install\moonlight"

  # Upstream defaults to BUILD_SHARED_LIBS=ON (libmoonlight-common-c.so); enet is a
  # static submodule folded in. No install() rules, so artifact/header are collected by hand.
  Invoke-CMake -Name moonlight -Src $src -Build $build -Options @(
    "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
    "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    # find_package(OpenSSL 1.0.2 REQUIRED) must resolve to the OHOS build, not a
    # host OpenSSL; libcrypto.so is the one OpenSSL in the process.
    "-DOPENSSL_ROOT_DIR=$Install\openssl",
    "-DOPENSSL_INCLUDE_DIR=$Install\openssl\include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Install\openssl\lib\libcrypto.so",
    "-DOPENSSL_SSL_LIBRARY=$Install\openssl\lib\libssl.so",
    "-DOPENSSL_USE_STATIC_LIBS=OFF"
  )

  if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
  New-Item -ItemType Directory -Path "$out\include" -Force | Out-Null
  Copy-Item -LiteralPath "$src\src\Limelight.h" -Destination "$out\include\" -Force

  Copy-PrebuiltLib "$build\libmoonlight-common-c.so"

  Sync-Moonlight
}

# Only the public header is synced next to the module; libraries ship from entry/libs/<abi>/.
function Sync-Moonlight {
  $from = "$Install\moonlight"
  $to = "$Thirdparty\moonlight"
  Assert-Found "$from\include\Limelight.h" "moonlight install (build moonlight first)"

  New-Item -ItemType Directory -Path "$to\include" -Force | Out-Null
  Copy-Item -Path "$from\include\*" -Destination "$to\include" -Recurse -Force

  $stale = "$to\lib"
  if (Test-Path -LiteralPath $stale) { Remove-Item -Recurse -Force -LiteralPath $stale }

  $now = Get-Date
  Get-ChildItem -LiteralPath $to -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
  Write-Host "=== moonlight headers synced to $to ==="
}

if ($Only -contains "zlib")      { Build-Zlib }
if ($Only -contains "openssl")   { Build-OpenSsl }
if ($Only -contains "opus")      { Build-Opus }
if ($Only -contains "freerdp")   { Build-FreeRdp }
if ($Only -contains "moonlight") { Build-Moonlight }

Write-Host "=== native build complete ($Arch): $($Only -join ', ') ==="
