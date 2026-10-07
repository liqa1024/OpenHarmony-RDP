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
    zlib      shared libz.so
    openssl   shared libcrypto.so / libssl.so (CMake + frozen Configure output)
    freerdp   shared libX.so; links zlib + OpenSSL dynamically
    moonlight shared libmoonlight-common-c.so (enet/nanors fold into it)

  Every independent dependency is a shared library and ships from
  entry/libs/<abi>/, so a library with more than one consumer in the process
  exists once instead of once per consumer (doc_agent/native-libraries.md); a
  submodule that is only an implementation detail of its parent (moonlight's
  enet/nanors) stays static inside that parent. Then synced next to the module
  (all gitignored, generated):
    entry/src/main/cpp/thirdparty/{freerdp,moonlight}/include
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
# The module packages entry/libs/<abi>/ into the HAP and the app's CMakeLists
# links every third-party library from there by full path.
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
    # RelWithDebInfo, not Release: the code is identical (-O2 either way, and the
    # optimisation level each project pins is what wins), but -g keeps native
    # stack frames symbolicable. The module strips before packaging, so this costs
    # nothing in the HAP. Minimum is -O2: -O0 output has no usable debug value
    # here. See doc_agent/native-libraries.md.
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

# Ships one shared library from a dependency's install prefix to
# entry/libs/<abi>/. Copy by name so a stale file from an earlier build is always
# replaced instead of accumulating.
function Copy-PrebuiltLib([string]$From) {
  if (-not (Test-Path -LiteralPath $From)) { throw "shared library not found: $From" }
  New-Item -ItemType Directory -Path $Libs -Force | Out-Null
  Copy-Item -LiteralPath $From -Destination $Libs -Force
  Write-Host "  -> $(Split-Path -Leaf $From)"
}

# --- zlib -------------------------------------------------------------------
function Build-Zlib {
  $src = "$Third\zlib-1.3.1"
  Assert-Found "$src\CMakeLists.txt" "zlib source (run fetch-sources.ps1)"
  Invoke-CMake -Name zlib -Src $src -Build "$BuildRoot\zlib-$Arch" -Prefix "$Install\zlib" -Options @(
    # Shared libz.so, shipped from entry/libs/<abi>/ like every other dependency;
    # FreeRDP records it in DT_NEEDED instead of folding a copy in.
    "-DBUILD_SHARED_LIBS=ON",
    "-DZLIB_BUILD_EXAMPLES=OFF"
  )
  Copy-PrebuiltLib "$Install\zlib\lib\libz.so"
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

# libcrypto.so / libssl.so are the single OpenSSL in the process: libwinpr3.so,
# libfreerdp3.so and libmoonlight-common-c.so all record them in DT_NEEDED. No
# consumer carries its own copy and no header set is synced next to the module
# (nothing under entry/src/main/cpp includes <openssl/...>).
function Sync-OpenSsl {
  $from = "$Install\openssl\lib"
  Assert-Found "$from\libcrypto.so" "OpenSSL install (build openssl first)"
  Assert-Found "$from\libssl.so" "OpenSSL install (build openssl first)"

  Copy-PrebuiltLib "$from\libcrypto.so"
  Copy-PrebuiltLib "$from\libssl.so"

  # Environments built before the shared switch kept headers + libcrypto.a here;
  # nothing reads them any more.
  $stale = "$Thirdparty\openssl"
  if (Test-Path -LiteralPath $stale) { Remove-Item -Recurse -Force -LiteralPath $stale }

  Write-Host "=== OpenSSL shared libraries copied to $Libs ==="
}

# --- freerdp ----------------------------------------------------------------
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
    # WINPR_ASSERT must never abort the host app: with the default (ON) any
    # internal FreeRDP sanity check (e.g. the audio backend failing to open a
    # device on OHOS) calls abort() and takes the whole process down. Turning it
    # off makes WINPR_ASSERT a plain assert(), disabled by NDEBUG, so FreeRDP's
    # error paths run instead of crashing.
    "-DWITH_VERBOSE_WINPR_ASSERT=OFF",
    "-DZLIB_LIBRARY=$zlib\lib\libz.so",
    "-DZLIB_INCLUDE_DIR=$zlib\include",
    # OpenSSL and zlib are the shared libraries installed above; FreeRDP records
    # them in DT_NEEDED instead of folding a private copy into libwinpr3.so /
    # libfreerdp3.so (the reason those two used to export ~5900 OpenSSL symbols
    # each and interpose on one another).
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
  Get-ChildItem -LiteralPath "$Install\freerdp\lib" -Filter *.so | ForEach-Object {
    Copy-PrebuiltLib $_.FullName
  }
  Write-Host "=== FreeRDP shared libraries copied to $Libs ==="
}

# --- moonlight --------------------------------------------------------------
function Build-Moonlight {
  $src = "$Third\moonlight-common-c"
  Assert-Found "$src\CMakeLists.txt" "moonlight-common-c source (run fetch-sources.ps1)"
  Assert-Found "$Install\openssl\lib\libcrypto.so" "OpenSSL (build openssl first)"
  $build = "$BuildRoot\moonlight-$Arch"
  $out = "$Install\moonlight"

  # Upstream CMakeLists defaults to BUILD_SHARED_LIBS=ON (line ~26), so nothing is
  # passed here: libmoonlight-common-c.so is what upstream means to build. enet is
  # its own submodule, not a project-level dependency - it hard-codes
  # add_library(enet STATIC) and stays that way, so it is folded into
  # libmoonlight-common-c.so instead of shipping as a second library.
  # There are no install() rules, so the artifact and header are collected by hand
  # below.
  Invoke-CMake -Name moonlight -Src $src -Build $build -Options @(
    "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
    "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
    # find_package(OpenSSL 1.0.2 REQUIRED) must resolve to the OHOS build, not a
    # host OpenSSL (there is none for OHOS). The paths also keep it off the
    # shared/static probe: libcrypto.so is the one OpenSSL in the process.
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

# Only the public header is synced next to the module: the libraries ship from
# entry/libs/<abi>/ like every other dependency.
function Sync-Moonlight {
  $from = "$Install\moonlight"
  $to = "$Thirdparty\moonlight"
  Assert-Found "$from\include\Limelight.h" "moonlight install (build moonlight first)"

  New-Item -ItemType Directory -Path "$to\include" -Force | Out-Null
  Copy-Item -Path "$from\include\*" -Destination "$to\include" -Recurse -Force

  # Environments built before the shared switch kept the archives here.
  $stale = "$to\lib"
  if (Test-Path -LiteralPath $stale) { Remove-Item -Recurse -Force -LiteralPath $stale }

  $now = Get-Date
  Get-ChildItem -LiteralPath $to -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
  Write-Host "=== moonlight headers synced to $to ==="
}

# --- run --------------------------------------------------------------------
if ($Only -contains "zlib")      { Build-Zlib }
if ($Only -contains "openssl")   { Build-OpenSsl }
if ($Only -contains "freerdp")   { Build-FreeRdp }
if ($Only -contains "moonlight") { Build-Moonlight }

Write-Host "=== native build complete ($Arch): $($Only -join ', ') ==="
