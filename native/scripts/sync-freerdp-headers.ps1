# Copies the FreeRDP/winpr headers the app compiles against out of the local
# FreeRDP install into entry/src/main/cpp/thirdparty/freerdp/.
#
# That directory is gitignored (it is generated, like entry/libs/<abi>/): the
# app build needs the headers next to the module, and they must not embed
# machine-local values, so this script is what puts them there. build-freerdp.ps1
# calls it at the end of every arch build, i.e. after the install prefix below
# has been created.
#
#   native/scripts/sync-freerdp-headers.ps1 [-Arch arm64-v8a]
param(
	[string]$Arch = "arm64-v8a"
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $Native "install\$Arch\freerdp\include"
$Dst = Join-Path (Split-Path -Parent $Native) "entry\src\main\cpp\thirdparty\freerdp\include"

if (-not (Test-Path -LiteralPath $Src)) {
	throw "FreeRDP install headers not found: $Src`nBuild FreeRDP first (native/scripts/build-freerdp.ps1 -Arch $Arch)."
}

# Mirror the install's include tree: the app must compile against exactly what
# was installed, with nothing stale left behind.
if (Test-Path -LiteralPath $Dst) {
	Remove-Item -Recurse -Force -LiteralPath $Dst
}
New-Item -ItemType Directory -Path $Dst -Force | Out-Null
Copy-Item -Path (Join-Path $Src "*") -Destination $Dst -Recurse -Force

# Copy-Item preserves the source timestamps; the module build tracks header
# changes by mtime, so make the refreshed headers look new.
$now = Get-Date
Get-ChildItem -LiteralPath $Dst -Recurse -File | ForEach-Object {
	$_.LastWriteTime = $now
}

# Normalize the values CMake bakes into the generated headers so what lands in
# the module carries no build-machine paths or local git revision.
function Replace-Line {
	param([string]$Path, [string]$Pattern, [string]$Replacement)
	if (-not (Test-Path -LiteralPath $Path)) {
		return
	}
	$raw = [System.IO.File]::ReadAllText($Path)
	$new = $raw -replace $Pattern, $Replacement
	if ($new -ne $raw) {
		[System.IO.File]::WriteAllText($Path, $new)
	}
}

Replace-Line (Join-Path $Dst "winpr3\winpr\build-config.h") `
	'(?m)^#define WINPR_INSTALL_PREFIX .*$' '#define WINPR_INSTALL_PREFIX "freerdp"'
Replace-Line (Join-Path $Dst "winpr3\winpr\build-config.h") `
	'(?m)^#define WINPR_INSTALL_SYSCONFDIR .*$' '#define WINPR_INSTALL_SYSCONFDIR "freerdp/etc"'
Replace-Line (Join-Path $Dst "freerdp3\freerdp\version.h") `
	'(?m)^#define FREERDP_GIT_REVISION .*$' '#define FREERDP_GIT_REVISION "n/a"'
Replace-Line (Join-Path $Dst "winpr3\winpr\version.h") `
	'(?m)^#define WINPR_GIT_REVISION .*$' '#define WINPR_GIT_REVISION "n/a"'

Write-Host "=== FreeRDP headers synced to $Dst ==="
Get-ChildItem -LiteralPath $Dst -Recurse -File | Measure-Object | Select-Object -ExpandProperty Count | ForEach-Object {
	Write-Host "=== $_ header files ==="
}
