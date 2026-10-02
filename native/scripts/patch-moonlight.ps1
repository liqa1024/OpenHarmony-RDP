# Applies the HmRdp patches to the moonlight-common-c source tree.
#
# The tree under native/third_party/ is gitignored and recreated by
# fetch-sources.ps1, which calls this script after extraction. Like
# patch-freerdp.ps1 it is idempotent: an already-patched tree reports the step
# as skipped instead of failing.
#
#   native/scripts/patch-moonlight.ps1 [-Source <tree>]
param(
	[string]$Source = ""
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
if ($Source.Length -eq 0) {
	$Source = Join-Path $Native "third_party\moonlight-common-c"
}
$CMake = Join-Path $Source "CMakeLists.txt"

if (-not (Test-Path -LiteralPath $CMake)) {
	throw "moonlight-common-c CMakeLists not found: $CMake"
}

# Read raw and write back with the file's own line endings preserved: a plain
# string replace does not touch them, so LF (tarball) and CRLF (checkout) both work.
$raw = [System.IO.File]::ReadAllText($CMake)

# 1) Drop the unconditional -Werror the upstream CMakeLists adds on non-MSVC /
#    non-MinGW platforms. The OHOS clang does not carry the warning set upstream
#    CI does, and -Werror is added *after* CMAKE_C_FLAGS so it cannot be turned
#    off from the command line. Warnings stay visible; they just do not fail the
#    build.
if ($raw -notmatch [regex]::Escape(" -Werror)")) {
	Write-Host "moonlight: -Werror already stripped (skipped)"
}
else {
	$patched = $raw -replace [regex]::Escape(" -Werror)"), ")"
	if ($patched -eq $raw) {
		throw "moonlight: -Werror present but not matched"
	}
	[System.IO.File]::WriteAllText($CMake, $patched)
	Write-Host "moonlight: stripped -Werror from CMakeLists.txt"
}

Write-Host "=== moonlight-common-c patches applied ==="
