# Makes zlib's shared library version-less (same SONAME/file) so DT_NEEDED matches
# the packaged file; idempotent.
param(
	[string]$Source = ""
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
if ($Source.Length -eq 0) {
	$Source = Join-Path $Native "third_party\zlib-1.3.1"
}
$CMake = Join-Path $Source "CMakeLists.txt"

if (-not (Test-Path -LiteralPath $CMake)) {
	throw "zlib CMakeLists not found: $CMake"
}

# Read raw / write back unchanged: single-line replacements are EOL-agnostic.
$raw = [System.IO.File]::ReadAllText($CMake)

# Drop both: CMake derives the SONAME from VERSION when SOVERSION is absent.
if ($raw -notmatch [regex]::Escape("SOVERSION 1")) {
	Write-Host "zlib: already version-less (skipped)"
}
else {
	$patched = $raw.Replace(
		'set_target_properties(zlib PROPERTIES SOVERSION 1)',
		'# HmRdp: no SOVERSION - one version-less libz.so (patch-zlib.ps1)')
	$patched = $patched -replace '(?m)^[ \t]*set_target_properties\(zlib PROPERTIES VERSION \$\{ZLIB_FULL_VERSION\}\)',
		'    # HmRdp: no VERSION - one version-less libz.so (patch-zlib.ps1)'
	if ($patched -eq $raw -or $patched -match [regex]::Escape("SOVERSION 1")) {
		throw "zlib: SOVERSION present but not matched"
	}
	[System.IO.File]::WriteAllText($CMake, $patched)
	Write-Host "zlib: dropped SOVERSION/VERSION from CMakeLists.txt"
}

Write-Host "=== zlib patches applied ==="
