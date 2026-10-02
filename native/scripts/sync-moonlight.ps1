# Copies the moonlight-common-c archives and public header the app compiles and
# links against out of the local moonlight build into
# entry/src/main/cpp/thirdparty/moonlight/.
#
# That directory is gitignored (generated, like the FreeRDP headers and
# entry/libs/<abi>/): the app build needs the archives next to the module.
# build-moonlight.ps1 calls this at the end of every arch build.
#
#   native/scripts/sync-moonlight.ps1 [-Arch arm64-v8a]
param(
	[string]$Arch = "arm64-v8a"
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $Native "install\$Arch\moonlight"
$Dst = Join-Path (Split-Path -Parent $Native) "entry\src\main\cpp\thirdparty\moonlight"

if (-not (Test-Path -LiteralPath "$Src\include\Limelight.h")) {
	throw "moonlight install not found: $Src`nBuild it first (native/scripts/build-moonlight.ps1 -Arch $Arch)."
}

# Keep other ABIs already synced: only the header dir and this arch's lib dir are
# replaced, so building arm64 then x86_64 leaves both under thirdparty/moonlight.
New-Item -ItemType Directory -Path "$Dst\include" -Force | Out-Null
Copy-Item -Path "$Src\include\*" -Destination "$Dst\include" -Recurse -Force

New-Item -ItemType Directory -Path "$Dst\lib\$Arch" -Force | Out-Null
Copy-Item -LiteralPath "$Src\lib\libmoonlight-common-c.a" -Destination "$Dst\lib\$Arch\" -Force
Copy-Item -LiteralPath "$Src\lib\libenet.a" -Destination "$Dst\lib\$Arch\" -Force

# OpenSSL's libcrypto is synced under thirdparty/openssl/ instead (see
# sync-openssl.ps1): moonlight references it, but the module links it once for
# both the client and the HTTP/pairing layer. Drop any copy an older sync left.
$staleCrypto = Join-Path $Dst "lib\$Arch\libcrypto.a"
if (Test-Path -LiteralPath $staleCrypto) {
	Remove-Item -LiteralPath $staleCrypto -Force
}

# Copy-Item preserves the source timestamps; the module build tracks header
# changes by mtime, so make the refreshed files look new.
$now = Get-Date
Get-ChildItem -LiteralPath $Dst -Recurse -File | ForEach-Object {
	$_.LastWriteTime = $now
}

Write-Host "=== moonlight archives synced to $Dst ==="
Get-ChildItem -LiteralPath $Dst -Recurse -File | Select-Object -ExpandProperty FullName
