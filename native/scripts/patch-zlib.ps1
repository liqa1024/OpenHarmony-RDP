# Applies the HmRdp patch to the zlib source tree.
#
# One step: drop the SOVERSION/VERSION properties so the shared library is a
# single version-less libz.so whose SONAME is also libz.so. FreeRDP is built the
# same way (patch-steps/05-unversioned-sonames.ps1), and a version-less SONAME is
# what makes DT_NEEDED match the packaged file exactly instead of shipping
# libz.so.1 next to a libz.so copy (see doc_agent/native-libraries.md).
#
# The tree under native/third_party/ is gitignored and recreated by
# fetch-sources.ps1, which calls this script after extraction. Like
# patch-freerdp.ps1 it is idempotent: an already-patched tree reports the step as
# skipped instead of failing.
#
#   native/scripts/patch-zlib.ps1 [-Source <tree>]
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

# Read raw and write back with the file's own line endings preserved: the
# replacements below are single lines, so LF (tarball) and CRLF both work.
$raw = [System.IO.File]::ReadAllText($CMake)

# Drop both properties: CMake derives the SONAME from SOVERSION, or from VERSION
# when SOVERSION is absent, so removing only one still yields a versioned name.
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
