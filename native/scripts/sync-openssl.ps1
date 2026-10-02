# Copies the OpenSSL headers the app compiles against out of the local OpenSSL
# build into entry/src/main/cpp/thirdparty/openssl/.
#
# Only the headers are synced, not the static archives. The OpenSSL *code* the
# Moonlight HTTP/pairing layer (and moonlight-common-c's pairing AES) links is
# the copy already inside libwinpr3.so: FreeRDP statically builds OpenSSL into
# winpr3 and those symbols are exported, so libhmrdp resolves SSL_* / EVP_* /
# X509_* against it. Linking a second copy of libssl/libcrypto would only add a
# duplicate OpenSSL to the process (and a few MB to the HAP) - the linker binds
# to winpr3 first either way. See doc_agent/native-libraries.md.
#
# That directory is gitignored (generated, like the FreeRDP headers, the
# moonlight archives and entry/libs/<abi>/): the app build needs the headers next
# to the module.
#
#   native/scripts/sync-openssl.ps1 [-Arch arm64-v8a]
param(
	[string]$Arch = "arm64-v8a"
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $Native "install\$Arch\openssl"
$Dst = Join-Path (Split-Path -Parent $Native) "entry\src\main\cpp\thirdparty\openssl"

if (-not (Test-Path -LiteralPath "$Src\include\openssl\ssl.h")) {
	throw "OpenSSL install not found: $Src`nBuild it first (native/scripts/build-openssl-wsl.sh $Arch)."
}

New-Item -ItemType Directory -Path "$Dst\include" -Force | Out-Null
Copy-Item -Path "$Src\include\*" -Destination "$Dst\include" -Recurse -Force

# Drop any lib/ an earlier version of this script left behind: the archives are
# not linked and must not look like they are.
$staleLib = Join-Path $Dst "lib"
if (Test-Path -LiteralPath $staleLib) {
	Remove-Item -Recurse -Force $staleLib
}

# Copy-Item preserves the source timestamps; the module build tracks header
# changes by mtime, so make the refreshed files look new.
$now = Get-Date
Get-ChildItem -LiteralPath $Dst -Recurse -File | ForEach-Object {
	$_.LastWriteTime = $now
}

Write-Host "=== OpenSSL headers synced to $Dst ==="
