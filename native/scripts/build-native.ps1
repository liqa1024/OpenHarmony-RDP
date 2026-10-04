<#
.SYNOPSIS
  Builds every OHOS native dependency from the pinned sources and syncs what the
  app module consumes.

.DESCRIPTION
  Single entry point for the native build. All steps are CMake against the
  Windows OHOS NDK - OpenSSL included: its `perl Configure` output is frozen
  under native/openssl-cmake/generated/, so no WSL/Perl is involved (see
  doc_agent/native-libraries.md).

  Built in dependency order:
    zlib      static archives; statically linked into FreeRDP / libhmrdp
    openssl   static libcrypto.a / libssl.a (CMake + frozen Configure output)
    freerdp   shared libX.so; statically links zlib + OpenSSL
    moonlight static archives (moonlight-common-c + enet)

  Then synced next to the module (both gitignored, generated):
    entry/src/main/cpp/thirdparty/{freerdp,openssl,moonlight}
    entry/libs/<abi>/*.so

  Prerequisite: fetch-sources.ps1 (pinned sources + patches).

.PARAMETER Arch
  arm64-v8a (default, device) or x86_64 (emulator).

.PARAMETER Clean
  Remove the build dir and install prefix of every step being built.

.PARAMETER Only
  Build only these steps. Dependencies are NOT pulled in automatically, so the
  prerequisites must already be installed.

.EXAMPLE
  native/scripts/build-native.ps1
  native/scripts/build-native.ps1 -Arch x86_64
  native/scripts/build-native.ps1 -Only freerdp -Clean      # FreeRDP patch loop
#>
param(
  [string]$Arch = "arm64-v8a",
  [switch]$Clean,
  [ValidateSet("zlib", "openssl", "freerdp", "moonlight")]
  [string[]]$Only = @("zlib", "openssl", "freerdp", "moonlight")
)
$ErrorActionPreference = "Stop"

# --- shared setup -----------------------------------------------------------
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

# --- helpers ----------------------------------------------------------------
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
    "-DCMAKE_BUILD_TYPE=Release"
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

# Mirror a directory, then make the copies look new: the module build tracks
# header changes by mtime.
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

# --- zlib -------------------------------------------------------------------
function Build-Zlib {
  $src = "$Third\zlib-1.3.1"
  Assert-Found "$src\CMakeLists.txt" "zlib source (run fetch-sources.ps1)"
  Invoke-CMake -Name zlib -Src $src -Build "$BuildRoot\zlib-$Arch" -Prefix "$Install\zlib" -Options @(
    # Static libz.a: FreeRDP links zlib statically, so it never ships. -fPIC is
    # required because it ends up inside the shared libfreerdp3.so / libhmrdp.so.
    "-DBUILD_SHARED_LIBS=OFF",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DZLIB_BUILD_EXAMPLES=OFF"
  )
  Write-Host "=== zlib ($Arch) installed to $Install\zlib ==="
}

# --- openssl ----------------------------------------------------------------
function Build-OpenSsl {
  $src = "$Native\openssl-cmake"
  Assert-Found "$Third\openssl-3.0.15\Configure" "OpenSSL source (run fetch-sources.ps1)"
  Assert-Found "$src\generated\sources.cmake" "OpenSSL snapshot (run export-openssl-snapshot.ps1)"

  Invoke-CMake -Name openssl -Src $src -Build "$BuildRoot\openssl-cmake-$Arch" -Prefix "$Install\openssl" -Options @(
    "-DOSSL_SRC=$Third\openssl-3.0.15"
  )
  Sync-OpenSsl
}

# libcrypto.a is what libhmrdp links for moonlight-common-c's control-stream
# crypto; its own copy, not the one FreeRDP statically links into libwinpr3.so
# (see doc_agent/native-libraries.md). Only the headers libhmrdp compiles against
# are synced alongside it.
function Sync-OpenSsl {
  $from = "$Install\openssl\include"
  $to = "$Thirdparty\openssl\include"
  Assert-Found "$from\openssl\ssl.h" "OpenSSL install (build openssl first)"

  New-Item -ItemType Directory -Path $to -Force | Out-Null
  Copy-Item -Path "$from\*" -Destination $to -Recurse -Force

  # Archive is per-ABI; keep the other ABI already under thirdparty/openssl/lib.
  $libTo = "$Thirdparty\openssl\lib\$Arch"
  New-Item -ItemType Directory -Path $libTo -Force | Out-Null
  Copy-Item -LiteralPath "$Install\openssl\lib\libcrypto.a" -Destination "$libTo\" -Force

  $now = Get-Date
  Get-ChildItem -LiteralPath "$Thirdparty\openssl" -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
  Write-Host "=== OpenSSL headers + libcrypto.a synced to $Thirdparty\openssl ==="
}

# --- freerdp ----------------------------------------------------------------
function Build-FreeRdp {
  $src = "$Third\FreeRDP"
  Assert-Found "$src\CMakeLists.txt" "FreeRDP source (run fetch-sources.ps1)"
  Assert-Found "$Install\openssl\lib\libcrypto.a" "OpenSSL (build openssl first)"
  $zlib = "$Install\zlib"
  $prefix = "$Install\freerdp"

  Invoke-CMake -Name freerdp -Src $src -Build "$BuildRoot\freerdp-$Arch" -Prefix $prefix -Options @(
    "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
    "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    "-DCMAKE_CXX_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    # WINPR_ASSERT must never abort the host app: with the default (ON) any
    # internal FreeRDP sanity check (e.g. the audio backend failing to open a
    # device on OHOS) calls abort() and takes the whole process down. Turning it
    # off makes WINPR_ASSERT a plain assert(), disabled by NDEBUG, so FreeRDP's
    # error paths run instead of crashing.
    "-DWITH_VERBOSE_WINPR_ASSERT=OFF",
    "-DZLIB_LIBRARY=$zlib\lib\libz.a",
    "-DZLIB_INCLUDE_DIR=$zlib\include",
    "-DOPENSSL_ROOT_DIR=$Install\openssl",
    "-DOPENSSL_INCLUDE_DIR=$Install\openssl\include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Install\openssl\lib\libcrypto.a",
    "-DOPENSSL_SSL_LIBRARY=$Install\openssl\lib\libssl.a",
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
    # WITH_SIMD=ON brings in FreeRDP's own SSE/NEON implementations (the
    # Progressive decode's inverse DWT, YCbCr->RGB, YCoCg, the SSE3/NEON
    # primitives). Those are not required to be bit-identical to the generic C
    # code, so the correctness criterion is a measured one (gfx-engine.md
    # §7.1/§7.2). Changing this switch invalidates the recorded reference frames.
    "-DWITH_SIMD=ON", "-DWITH_AAD=OFF", "-DWITH_SMARTCARD=OFF",
    "-DWITH_KEYBOARD_LAYOUT_FROM_FILE=OFF",
    "-DCHANNEL_AUDIN=ON", "-DCHANNEL_ENCOMSP=OFF", "-DCHANNEL_RAIL=OFF", "-DCHANNEL_REMDESK=OFF",
    "-DCHANNEL_TELEMETRY=OFF", "-DCHANNEL_URBDRC=OFF", "-DCHANNEL_SMARTCARD=OFF",
    "-DCHANNEL_CLIPRDR=ON", "-DCHANNEL_RDPDR=ON", "-DCHANNEL_RDPSND=ON", "-DCHANNEL_RDPSND_CLIENT=ON",
    "-DCHANNEL_DRDYNVC=ON", "-DCHANNEL_DISP=ON", "-DCHANNEL_RDPGFX=ON", "-DCHANNEL_RDPEI=ON",
    "-DCHANNEL_GEOMETRY=OFF", "-DCHANNEL_VIDEO=OFF",
    "-DWITH_WINPR_TOOLS=OFF", "-DWITH_BINARY_VERSIONING=OFF", "-DCMAKE_SKIP_INSTALL_RPATH=ON",
    # A single version-less libX.so (SONAME libX.so) instead of libX.so.3; the
    # OHOS patch to cmake/AddTargetWithResourceFile.cmake keeps the "lib" prefix
    # and sets the SONAME on non-Windows.
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
  # Normalize the values CMake bakes into the generated headers: the module must
  # carry no build-machine paths or local git revision.
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
  $dst = "$Repo\entry\libs\$Arch"
  New-Item -ItemType Directory -Path $dst -Force | Out-Null
  Copy-Item -Path "$Install\freerdp\lib\*.so" -Destination $dst -Force
  Write-Host "=== FreeRDP shared libraries copied to $dst ==="
}

# --- moonlight --------------------------------------------------------------
function Build-Moonlight {
  $src = "$Third\moonlight-common-c"
  Assert-Found "$src\CMakeLists.txt" "moonlight-common-c source (run fetch-sources.ps1)"
  Assert-Found "$Install\openssl\lib\libcrypto.a" "OpenSSL (build openssl first)"
  $build = "$BuildRoot\moonlight-$Arch"
  $out = "$Install\moonlight"

  # The upstream CMakeLists has no install() rules, so the archives and header
  # are collected by hand below.
  Invoke-CMake -Name moonlight -Src $src -Build $build -Options @(
    "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
    "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    # Both the library and enet are PIC so the archives can go inside libhmrdp.so.
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DBUILD_SHARED_LIBS=OFF",
    # find_package(OpenSSL 1.0.2 REQUIRED) needs the static crypto FreeRDP
    # already uses; the paths bypass a system OpenSSL (there is none for OHOS).
    "-DOPENSSL_ROOT_DIR=$Install\openssl",
    "-DOPENSSL_INCLUDE_DIR=$Install\openssl\include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Install\openssl\lib\libcrypto.a",
    "-DOPENSSL_SSL_LIBRARY=$Install\openssl\lib\libssl.a",
    "-DOPENSSL_USE_STATIC_LIBS=ON"
  )

  if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
  New-Item -ItemType Directory -Path "$out\lib" -Force | Out-Null
  New-Item -ItemType Directory -Path "$out\include" -Force | Out-Null
  Copy-Item -LiteralPath "$build\libmoonlight-common-c.a" -Destination "$out\lib\" -Force
  Copy-Item -LiteralPath "$build\enet\libenet.a" -Destination "$out\lib\" -Force
  Copy-Item -LiteralPath "$src\src\Limelight.h" -Destination "$out\include\" -Force

  Sync-Moonlight
}

# moonlight archives are per-ABI; keep the other ABI already under thirdparty/.
function Sync-Moonlight {
  $from = "$Install\moonlight"
  $to = "$Thirdparty\moonlight"
  Assert-Found "$from\include\Limelight.h" "moonlight install (build moonlight first)"

  New-Item -ItemType Directory -Path "$to\include" -Force | Out-Null
  Copy-Item -Path "$from\include\*" -Destination "$to\include" -Recurse -Force
  New-Item -ItemType Directory -Path "$to\lib\$Arch" -Force | Out-Null
  Copy-Item -LiteralPath "$from\lib\libmoonlight-common-c.a" -Destination "$to\lib\$Arch\" -Force
  Copy-Item -LiteralPath "$from\lib\libenet.a" -Destination "$to\lib\$Arch\" -Force

  # drop any libcrypto.a an older sync left next to the archives: OpenSSL now
  # lives under thirdparty/openssl/lib and is linked from there.
  $stale = "$to\lib\$Arch\libcrypto.a"
  if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }

  $now = Get-Date
  Get-ChildItem -LiteralPath $to -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
  Write-Host "=== moonlight archives synced to $to ==="
}

# --- run --------------------------------------------------------------------
if ($Only -contains "zlib")      { Build-Zlib }
if ($Only -contains "openssl")   { Build-OpenSsl }
if ($Only -contains "freerdp")   { Build-FreeRdp }
if ($Only -contains "moonlight") { Build-Moonlight }

Write-Host "=== native build complete ($Arch): $($Only -join ', ') ==="
