#!/bin/bash
# Cross-compile OpenSSL for HarmonyOS using the existing Windows OHOS NDK.
# Runs inside WSL (full perl + make + sh) but drives the Windows clang.exe,
# so no Linux OHOS SDK download is required.
#
# Usage: build-openssl-wsl.sh <src> <build> <prefix> [arm64-v8a|x86_64|armeabi-v7a]
set -e

SRC="$1"
BUILD="$2"
PREFIX="$3"
ARCH="${4:-arm64-v8a}"
JOBS="${JOBS:-$(nproc)}"

NDK_WIN="${NDK_WIN:-C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"
NDK_UNIX="${NDK_UNIX:-/mnt/c/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"

case "$ARCH" in
  arm64-v8a)   TARGET=aarch64-linux-ohos; OSSL_TARGET=linux-aarch64 ;;
  x86_64)      TARGET=x86_64-linux-ohos;  OSSL_TARGET=linux-x86_64 ;;
  armeabi-v7a) TARGET=arm-linux-ohos;     OSSL_TARGET=linux-armv4 ;;
  *) echo "ERROR: unsupported arch $ARCH" >&2; exit 1 ;;
esac

if [ ! -f "$SRC/Configure" ]; then
  echo "ERROR: OpenSSL source not found: $SRC" >&2
  exit 1
fi

rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD"

CCW="$BUILD/ohos-clang"
CXXW="$BUILD/ohos-clang++"

cat > "$CCW" <<EOF
#!/bin/sh
exec "$NDK_UNIX/llvm/bin/clang.exe" --target=$TARGET --sysroot="$NDK_WIN/sysroot" "\$@"
EOF

cat > "$CXXW" <<EOF
#!/bin/sh
exec "$NDK_UNIX/llvm/bin/clang++.exe" --target=$TARGET --sysroot="$NDK_WIN/sysroot" "\$@"
EOF

chmod +x "$CCW" "$CXXW"

export CC="$CCW"
export CXX="$CXXW"
if command -v ar >/dev/null 2>&1; then
  export AR="$(command -v ar)"
  export RANLIB="$(command -v ranlib)"
else
  export AR="$NDK_UNIX/llvm/bin/llvm-ar.exe"
  export RANLIB="$NDK_UNIX/llvm/bin/llvm-ranlib.exe"
fi

SRCCOPY="$BUILD/openssl"
cp -a "$SRC" "$SRCCOPY"
cd "$SRCCOPY"

perl Configure "$OSSL_TARGET" \
  --prefix="$PREFIX" \
  --openssldir=/dev/null \
  --libdir=lib \
  no-shared no-tests no-asm no-dso no-engine no-module no-autoload-config \
  -fPIC -O2 -DOPENSSL_NO_AUTOLOAD_CONFIG=1

make -j"$JOBS" build_libs
make install_dev

echo "=== OpenSSL ($ARCH) installed to $PREFIX ==="
ls -la "$PREFIX/lib"
