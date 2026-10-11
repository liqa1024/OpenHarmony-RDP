# Applies the HmRdp patch to the opus source tree.
#
# One step: drop the SOVERSION/VERSION properties so the shared library is a
# single version-less libopus.so whose SONAME is also libopus.so. zlib and
# FreeRDP are built the same way (patch-zlib.ps1, patch-steps/05-unversioned-
# sonames.ps1), and a version-less SONAME is what makes DT_NEEDED match the
# packaged file exactly instead of shipping libopus.so.0 next to a libopus.so
# copy (see doc_agent/native-build.md).
#
# The tree under native/third_party/ is gitignored and recreated by
# fetch-sources.ps1, which calls this script after extraction. Like
# patch-zlib.ps1 it is idempotent: an already-patched tree reports the step as
# skipped instead of failing.
#
#   native/scripts/patch-opus.ps1 [-Source <tree>]
param(
	[string]$Source = ""
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
if ($Source.Length -eq 0) {
	$Source = Join-Path $Native "third_party\opus-1.5.2"
}
$CMake = Join-Path $Source "CMakeLists.txt"

if (-not (Test-Path -LiteralPath $CMake)) {
	throw "opus CMakeLists not found: $CMake"
}

# Read raw and write back with the file's own line endings preserved: the block
# below is matched with the tree's own EOL, so LF (tarball) and CRLF both work.
$raw = [System.IO.File]::ReadAllText($CMake)
$eol = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }

# Drop both properties: CMake derives the SONAME from SOVERSION, or from VERSION
# when SOVERSION is absent, so removing only one still yields a versioned name.
if ($raw -notmatch "PROPERTIES SOVERSION") {
	Write-Host "opus: already version-less (skipped)"
}
else {
	$anchor = @(
		"set_target_properties(opus",
		"                      PROPERTIES SOVERSION",
		"                                 `${OPUS_LIBRARY_VERSION_MAJOR}",
		"                                 VERSION",
		"                                 `${OPUS_LIBRARY_VERSION}",
		"                                 PUBLIC_HEADER",
		"                                 `"`${Opus_PUBLIC_HEADER}`")"
	) -join $eol
	$replacement = @(
		"# HMRDP_OPUS: no SOVERSION/VERSION - one version-less libopus.so (patch-opus.ps1)",
		"set_target_properties(opus",
		"                      PROPERTIES PUBLIC_HEADER",
		"                                 `"`${Opus_PUBLIC_HEADER}`")"
	) -join $eol

	if (-not $raw.Contains($anchor)) {
		throw "opus: SOVERSION present but the set_target_properties block was not matched"
	}
	$patched = $raw.Replace($anchor, $replacement)
	[System.IO.File]::WriteAllText($CMake, $patched)
	Write-Host "opus: dropped SOVERSION/VERSION from CMakeLists.txt"
}

Write-Host "=== opus patch applied ==="
