param(
  [string]$Arch = "arm64-v8a",
  [switch]$Clean
)
$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Ndk = "C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native"
$Toolchain = "$Ndk\build\cmake\ohos.toolchain.cmake"
$Ninja = (Get-Command ninja).Source
switch ($Arch) {
  "arm64-v8a"   { }
  "armeabi-v7a" { }
  "x86_64"      { }
  default       { throw "unsupported arch: $Arch" }
}

$Src = "$Native\third_party\zlib-1.3.1"
$Build = "$Native\build\zlib-$Arch"
$Prefix = "$Native\install\$Arch\zlib"

if (-not (Test-Path -LiteralPath "$Src\CMakeLists.txt")) {
  throw "zlib source not found: $Src"
}

if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
if ($Clean -and (Test-Path $Prefix)) { Remove-Item -Recurse -Force $Prefix }

$cfg = @(
  "-S", $Src, "-B", $Build, "-G", "Ninja",
  "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
  "-DCMAKE_MAKE_PROGRAM=$Ninja",
  "-DOHOS_ARCH=$Arch",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DCMAKE_INSTALL_PREFIX=$Prefix",
  # Static libz.a: FreeRDP links zlib statically (see build-freerdp.ps1), so the
  # library itself never ships. -fPIC is required because it ends up inside the
  # shared libfreerdp3.so.
  "-DBUILD_SHARED_LIBS=OFF",
  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
  "-DZLIB_BUILD_EXAMPLES=OFF"
)

& cmake @cfg

& cmake --build $Build --parallel 8
& cmake --install $Build --prefix $Prefix

Write-Host "=== zlib ($Arch) installed to $Prefix ==="
Get-ChildItem "$Prefix\lib" -ErrorAction SilentlyContinue | Select-Object Name, Length
