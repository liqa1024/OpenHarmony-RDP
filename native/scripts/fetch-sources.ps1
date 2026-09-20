# Fetches the pinned third-party sources into native/third_party and applies the
# HmRdp patches.
#
# The source trees (and the downloaded archives) live under native/third_party/,
# which is gitignored: this script is what recreates them. Everything is pinned
# to an exact upstream release plus its SHA-256, so the same inputs always give
# the same tree.
#
#   native/scripts/fetch-sources.ps1            # fetch what is missing, then patch
#   native/scripts/fetch-sources.ps1 -Force     # re-download and re-extract everything
#
# FreeRDP is the only tree the patches touch (native/scripts/patch-freerdp.ps1);
# everything else is used as released. The patch helpers match on LF-normalized
# text, so the checkout style (LF from the tarball, CRLF from a Windows checkout)
# does not matter.
param(
	[switch]$Force
)

$ErrorActionPreference = "Stop"

$Native = Split-Path -Parent $PSScriptRoot
$Third = Join-Path $Native "third_party"
$Dl = Join-Path $Third "dl"

# Pinned releases. Url + Sha256 are the official artifacts; ArchiveRoot is the
# top-level directory the archive extracts to (FreeRDP's does not match Dir).
$Deps = @(
	[pscustomobject]@{
		Name        = "FreeRDP"
		Dir         = "FreeRDP"
		ArchiveRoot = "FreeRDP-3.10.3"
		Url         = "https://github.com/FreeRDP/FreeRDP/archive/refs/tags/3.10.3.tar.gz"
		Sha256      = "011B645E49401E59396DED91CCCF9A0CDF68E6C43A3CB0BF6A9B6852C9C564A4"
		Patch       = $true
	},
	[pscustomobject]@{
		Name        = "zlib"
		Dir         = "zlib-1.3.1"
		ArchiveRoot = "zlib-1.3.1"
		Url         = "https://zlib.net/zlib-1.3.1.tar.gz"
		Sha256      = "9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23"
		Patch       = $false
	},
	[pscustomobject]@{
		Name        = "OpenSSL"
		Dir         = "openssl-3.0.15"
		ArchiveRoot = "openssl-3.0.15"
		Url         = "https://www.openssl.org/source/old/3.0/openssl-3.0.15.tar.gz"
		Sha256      = "23C666D0EDF20F14249B3D8F0368ACAEE9AB585B09E1DE82107C66E1F3EC9533"
		Patch       = $false
	},
	# FSR 1.0 (MIT): the EASU/RCAS shader headers the Vulkan presenter #includes.
	# Header-only, so nothing is built from it - the app's CMake finds the tree at
	# native/third_party/fidelityfx-fsr-1.0.2 and embeds the compiled SPIR-V.
	[pscustomobject]@{
		Name        = "FidelityFX-FSR"
		Dir         = "fidelityfx-fsr-1.0.2"
		ArchiveRoot = "FidelityFX-FSR-1.0.2"
		Url         = "https://github.com/GPUOpen-Effects/FidelityFX-FSR/archive/refs/tags/v1.0.2.tar.gz"
		Sha256      = "92EE8F9630364EC3AD7FC16AF5A990601EBF18142571527ED672B79B79D04ACD"
		Patch       = $false
	}
)

New-Item -ItemType Directory -Path $Third -Force | Out-Null
New-Item -ItemType Directory -Path $Dl -Force | Out-Null

# Downloads (when needed) and verifies the pinned archive for one dependency.
function Get-PinnedArchive {
	param($Dep)
	$tgz = Join-Path $Dl ([System.IO.Path]::GetFileName($Dep.Url))
	if ($Force -or -not (Test-Path -LiteralPath $tgz)) {
		Write-Host "downloading $($Dep.Url)"
		Invoke-WebRequest -Uri $Dep.Url -OutFile $tgz -UseBasicParsing -TimeoutSec 600
	}
	$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $tgz).Hash
	if ($actual -ne $Dep.Sha256) {
		throw @"
SHA-256 mismatch for $tgz
  expected $($Dep.Sha256)
  actual   $actual
Delete the archive and retry; change the pin only together with the version.
"@
	}
	return $tgz
}

foreach ($dep in $Deps) {
	$target = Join-Path $Third $dep.Dir
	if ((Test-Path -LiteralPath $target) -and -not $Force) {
		Write-Host "$($dep.Name): $target already present (use -Force to re-fetch)"
		continue
	}

	$tgz = Get-PinnedArchive $dep
	$staging = Join-Path $Third (".$($dep.Dir).staging")
	if (Test-Path -LiteralPath $staging) {
		Remove-Item -Recurse -Force $staging
	}
	New-Item -ItemType Directory -Path $staging | Out-Null

	& tar -xzf $tgz -C $staging
	if ($LASTEXITCODE -ne 0) {
		throw "tar failed for $tgz"
	}

	$root = Join-Path $staging $dep.ArchiveRoot
	if (-not (Test-Path -LiteralPath $root)) {
		throw "expected $root after extracting $tgz"
	}

	if ($dep.ArchiveRoot -ne $dep.Dir) {
		Move-Item -LiteralPath $root -Destination (Join-Path $staging $dep.Dir)
	}

	if (Test-Path -LiteralPath $target) {
		Remove-Item -Recurse -Force $target
	}
	Move-Item -LiteralPath (Join-Path $staging $dep.Dir) -Destination $target
	Remove-Item -Recurse -Force $staging
	Write-Host "$($dep.Name): extracted to $target ($($dep.Dir))"
}

# Apply the FreeRDP patches last: the script is idempotent and reports each step
# it skipped ("already applied"), which is the self-check for an already-patched
# tree.
$freerdp = $Deps | Where-Object { $_.Patch } | Select-Object -First 1
$freerdpSrc = Join-Path $Third $freerdp.Dir
Write-Host "applying patches to $freerdpSrc"
& pwsh -NoProfile -File (Join-Path $PSScriptRoot "patch-freerdp.ps1") -Source $freerdpSrc
if ($LASTEXITCODE -ne 0) {
	throw "patch-freerdp.ps1 failed"
}

Write-Host ""
Write-Host "third-party sources are ready under $Third"
Write-Host "next: native/scripts/build-zlib.ps1 / build-openssl-wsl.sh / build-freerdp.ps1"
