param(
  [string]$Arch = "arm64-v8a",
  [switch]$Clean
)
$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Ndk = "C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native"
$Toolchain = "$Ndk\build\cmake\ohos.toolchain.cmake"
$Sysroot = "$Ndk\sysroot"
$Ninja = (Get-Command ninja).Source
$Triple = switch ($Arch) {
  "arm64-v8a"   { "aarch64-linux-ohos" }
  "armeabi-v7a" { "arm-linux-ohos" }
  "x86_64"      { "x86_64-linux-ohos" }
  default       { throw "unsupported arch: $Arch" }
}

$Src = "$Native\third_party\FreeRDP"
$Build = "$Native\build\freerdp-$Arch"
$Prefix = "$Native\install\$Arch\freerdp"
$OpenSsl = "$Native\install\$Arch\openssl"
$Zlib = "$Native\install\$Arch\zlib"

if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }

$cfg = @(
  "-S", $Src, "-B", $Build, "-G", "Ninja",
  "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
  "-DCMAKE_MAKE_PROGRAM=$Ninja",
  "-DOHOS_ARCH=$Arch",
  "-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON",
  "-DCMAKE_BUILD_TYPE=Release",
  "-DCMAKE_INSTALL_PREFIX=$Prefix",
  "-DCMAKE_C_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
  "-DCMAKE_CXX_FLAGS=-O2 -DNDEBUG -D__OHOS__=1",
  "-DZLIB_LIBRARY=$Zlib\lib\libz.a",
  "-DZLIB_INCLUDE_DIR=$Zlib\include",
  "-DOPENSSL_ROOT_DIR=$OpenSsl",
  "-DOPENSSL_INCLUDE_DIR=$OpenSsl\include",
  "-DOPENSSL_CRYPTO_LIBRARY=$OpenSsl\lib\libcrypto.a",
  "-DOPENSSL_SSL_LIBRARY=$OpenSsl\lib\libssl.a",
  "-DWITH_SERVER=OFF", "-DWITH_SAMPLE=OFF",
  "-DWITH_CLIENT=ON", "-DWITH_CLIENT_COMMON=ON", "-DWITH_CLIENT_INTERFACE=OFF",
  "-DWITH_PROXY=OFF", "-DWITH_SHADOW=OFF",
  "-DWITH_CUPS=OFF", "-DWITH_PULSE=OFF", "-DWITH_ALSA=OFF", "-DWITH_OSS=OFF",
  "-DWITH_FFMPEG=OFF", "-DWITH_SWSCALE=OFF",
  "-DWITH_X11=OFF", "-DWITH_WAYLAND=OFF",
  "-DWITH_GSTREAMER_0_10=OFF", "-DWITH_GSTREAMER_1_0=OFF",
  "-DWITH_LIBSYSTEMD=OFF", "-DWITH_PCSC=OFF", "-DWITH_JPEG=OFF",
  "-DWITH_OPENSLES=ON",
  "-DOpenSLES_INCLUDE_DIR=$Sysroot\usr\include",
  "-DOpenSLES_LIBRARY=$Sysroot\usr\lib\$Triple\libOpenSLES.so",
  "-DWITH_OPENH264=OFF",
  "-DWITH_GSM=OFF", "-DWITH_LAME=OFF", "-DWITH_FAAD2=OFF", "-DWITH_FAAC=OFF",
  "-DWITH_SOXR=OFF", "-DWITH_OPUS=OFF", "-DWITH_PKCS11=OFF", "-DWITH_ICU=OFF", "-DWITH_KRB5=OFF",
  "-DWITH_UNICODE_BUILTIN=ON", "-DWITH_INTERNAL_RC4=ON", "-DWITH_INTERNAL_MD4=ON", "-DWITH_INTERNAL_MD5=ON",
  "-DWITH_FUSE=OFF", "-DWITH_CLIENT_SDL=OFF", "-DWITH_MANPAGES=OFF",
  "-DBUILD_SHARED_LIBS=ON",
  "-DWITH_CHANNELS=ON", "-DWITH_CLIENT_CHANNELS=ON", "-DBUILTIN_CHANNELS=ON",
  "-DWITH_CAIRO=OFF", "-DWITH_SDL_IMAGE_DIALOGS=OFF", "-DWITH_WEBVIEW=OFF",
  "-DWITH_PLATFORM_SERVER=OFF", "-DWITH_PROGRESS_BAR=OFF",
  "-DWITH_SIMD=OFF", "-DWITH_NEON=OFF", "-DWITH_AAD=OFF", "-DWITH_SMARTCARD=OFF",
  "-DWITH_KEYBOARD_LAYOUT_FROM_FILE=OFF",
  "-DCHANNEL_AUDIN=ON", "-DCHANNEL_ENCOMSP=OFF", "-DCHANNEL_RAIL=OFF", "-DCHANNEL_REMDESK=OFF",
  "-DCHANNEL_TELEMETRY=OFF", "-DCHANNEL_URBDRC=OFF", "-DCHANNEL_SMARTCARD=OFF",
  "-DCHANNEL_CLIPRDR=ON", "-DCHANNEL_RDPDR=ON", "-DCHANNEL_RDPSND=ON", "-DCHANNEL_RDPSND_CLIENT=ON",
  "-DCHANNEL_DRDYNVC=ON", "-DCHANNEL_DISP=ON", "-DCHANNEL_RDPGFX=ON", "-DCHANNEL_RDPEI=ON",
  "-DCHANNEL_GEOMETRY=OFF", "-DCHANNEL_VIDEO=OFF",
  "-DWITH_WINPR_TOOLS=OFF", "-DWITH_BINARY_VERSIONING=OFF", "-DCMAKE_SKIP_INSTALL_RPATH=ON",
  "-DWITH_ABSOLUTE_PLUGIN_LOAD_PATHS=OFF",
  "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"
)

& cmake @cfg

& cmake --build $Build --parallel 8
& cmake --install $Build --prefix $Prefix

Write-Host "=== FreeRDP installed to $Prefix ==="
Get-ChildItem "$Prefix\lib" -Filter *.so* -ErrorAction SilentlyContinue | Select-Object Name, Length
