# Makes opus's shared library version-less (same SONAME/file) so DT_NEEDED matches
# the packaged file; idempotent.
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

# Read raw / write back preserving the file's own EOL: the anchor below uses that EOL.
$raw = [System.IO.File]::ReadAllText($CMake)
$eol = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }

# Drop both: CMake derives the SONAME from VERSION when SOVERSION is absent.
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
