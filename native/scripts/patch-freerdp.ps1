# Applies the HarmonyOS/musl compatibility patches to a FreeRDP source tree.
#
# Usage:  pwsh -File native/scripts/patch-freerdp.ps1 [-Source <path-to-FreeRDP>]
#
# The build pipeline expects the patched tree under native/third_party/FreeRDP
# (see native/scripts/build-freerdp.ps1). Run this script once after cloning
# FreeRDP 3.10.3 and before configuring the CMake build.
#
# The steps are one file each under native/scripts/patch-steps/, applied in name
# order; the numeric prefix is the step number the patched sources refer to in
# their comments (8 appears twice, 18 was tried and dropped again - see
# doc_agent/native-libraries.md). Every step must be idempotent: it carries a
# marker string and skips itself when the tree already has that change, so the
# script can be re-run over a tree that is already patched.
param(
	[string]$Source = "$PSScriptRoot\..\third_party\FreeRDP"
)

$ErrorActionPreference = "Stop"

# Data files the steps copy over the tree (backend sources, cmake helpers).
$PatchData = "$PSScriptRoot\..\patches"

function Patch-File {
	param([string]$Path, [hashtable]$Replacements)
	if (-not (Test-Path -LiteralPath $Path)) {
		throw "file not found: $Path"
	}
	$text = [System.IO.File]::ReadAllText($Path)
	foreach ($key in $Replacements.Keys) {
		$text = $text.Replace($key, $Replacements[$key])
	}
	[System.IO.File]::WriteAllText($Path, $text)
}

# Replaces a whole block of text: `Old` has to be found verbatim, `Marker` is what
# makes the step a no-op on an already patched tree.
function Patch-Block {
	param([string]$Path, [string]$Old, [string]$New, [string]$Marker)
	if (-not (Test-Path -LiteralPath $Path)) {
		throw "file not found: $Path"
	}
	$text = [System.IO.File]::ReadAllText($Path)
	if ($Marker -and $text.Contains($Marker)) {
		Write-Host "HmRdp tuning patch already applied to $(Split-Path -Leaf $Path)"
		return
	}
	if (-not $text.Contains($Old)) {
		throw "HmRdp tuning patch: block not found in $Path"
	}
	$text = $text.Replace($Old, $New)
	[System.IO.File]::WriteAllText($Path, $text)
	Write-Host "HmRdp tuning patch applied to $(Split-Path -Leaf $Path)"
}

# Tabs <n> <line> builds one C line with <n> tab indents; the line is passed as a
# literal (single-quoted) string, so C quotes need no PowerShell escaping.
function Tabs([int]$count, [string]$line) {
	return ("`t" * $count) + $line
}

# Regular-expression variant of Patch-Block, for the steps whose anchor has to
# absorb whatever version of the region is in the tree (see the step comments).
function Patch-Regex {
	param([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker)
	if (-not (Test-Path -LiteralPath $Path)) {
		throw "file not found: $Path"
	}
	$raw = [System.IO.File]::ReadAllText($Path)
	if ($Marker -and $raw.Contains($Marker)) {
		Write-Host "HmRdp progressive tuning already applied to $(Split-Path -Leaf $Path)"
		return
	}
	# Match on LF-normalized text: the replacement blocks are written with one
	# line-ending style, upstream files are not consistent. The file's own style is
	# restored on write.
	$crlf = $raw.Contains("`r`n")
	$text = $raw.Replace("`r`n", "`n")
	$Replacement = $Replacement.Replace("`r`n", "`n")
	$re = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
	if (-not $re.IsMatch($text)) {
		throw "HmRdp progressive tuning: pattern not found in $Path"
	}
	$evaluator = [System.Text.RegularExpressions.MatchEvaluator] { param($m) $Replacement }
	$text = $re.Replace($text, $evaluator, 1)
	if ($crlf) {
		$text = $text.Replace("`n", "`r`n")
	}
	[System.IO.File]::WriteAllText($Path, $text)
	Write-Host "HmRdp progressive tuning applied to $(Split-Path -Leaf $Path)"
}

# Apply every step in name order. Each one reports itself ("applied" / "already
# applied"), so a run over an already patched tree should print one line per step
# and nothing else.
$steps = Get-ChildItem -LiteralPath "$PSScriptRoot\patch-steps" -Filter *.ps1 | Sort-Object Name
foreach ($step in $steps) {
	. $step.FullName
}

Write-Host "FreeRDP OHOS patches applied to $Source"
