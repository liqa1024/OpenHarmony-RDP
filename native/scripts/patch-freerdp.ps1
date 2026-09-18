# Applies the HarmonyOS/musl compatibility patches to a FreeRDP source tree.
#
# Usage:  pwsh -File native/scripts/patch-freerdp.ps1 [-Source <path-to-FreeRDP>]
#
# The build pipeline expects the patched tree under native/third_party/FreeRDP
# (see native/scripts/build-freerdp.ps1). native/scripts/fetch-sources.ps1
# downloads the pinned upstream release and runs this script over it.
#
# The steps are one file each under native/scripts/patch-steps/, applied in name
# order; the numeric prefix is the step number the patched sources refer to in
# their comments (8 appears twice, 18 was tried and dropped again - see
# doc_agent/native-libraries.md). Every step must be idempotent: it carries a
# marker string and skips itself when the tree already has that change, so the
# script can be re-run over a tree that is already patched.
#
# All text handling is line-ending agnostic: upstream tarballs are LF while a
# Windows checkout is CRLF, and the anchors/replacements below are authored with
# one style only. Matching happens on LF-normalized text, and the file's own
# style is restored on write.
param(
	[string]$Source = "$PSScriptRoot\..\third_party\FreeRDP",
	[switch]$Trace
)

$ErrorActionPreference = "Stop"

# Data files the steps copy over the tree (backend sources, cmake helpers).
# `$Patches` is the historical name a few steps use; both point at the same dir.
$PatchData = "$PSScriptRoot\..\patches"
$Patches = $PatchData

function ConvertTo-LfText([string]$text) {
	if ($null -eq $text) {
		return ""
	}
	return $text.Replace("`r`n", "`n")
}

function Read-SourceText {
	param([string]$Path)
	if (-not (Test-Path -LiteralPath $Path)) {
		throw "file not found: $Path"
	}
	$raw = [System.IO.File]::ReadAllText($Path)
	[pscustomobject]@{
		Text = (ConvertTo-LfText $raw)
		Crlf = $raw.Contains("`r`n")
	}
}

function Write-SourceText {
	param([string]$Path, [string]$LfText, [bool]$Crlf)
	$out = $LfText
	if ($Crlf) {
		$out = $out.Replace("`n", "`r`n")
	}
	[System.IO.File]::WriteAllText($Path, $out)
}

# Literal replacements, without an anchor: any key that is not in the file is
# silently ignored (several steps apply the same map to files where only some
# identifiers exist).
function Patch-File {
	param([string]$Path, [hashtable]$Replacements)
	$f = Read-SourceText -Path $Path
	$text = $f.Text
	foreach ($key in $Replacements.Keys) {
		$text = $text.Replace((ConvertTo-LfText $key), (ConvertTo-LfText $Replacements[$key]))
	}
	Write-SourceText -Path $Path -LfText $text -Crlf $f.Crlf
}

# Replaces a whole block of text: `Old` has to be found verbatim, `Marker` is what
# makes the step a no-op on an already patched tree.
function Patch-Block {
	param([string]$Path, [string]$Old, [string]$New, [string]$Marker)
	$f = Read-SourceText -Path $Path
	if ($Marker -and $f.Text.Contains((ConvertTo-LfText $Marker))) {
		Write-Host "HmRdp tuning patch already applied to $(Split-Path -Leaf $Path)"
		return
	}
	$old = ConvertTo-LfText $Old
	if (-not $f.Text.Contains($old)) {
		throw "HmRdp tuning patch: block not found in $Path"
	}
	$text = $f.Text.Replace($old, (ConvertTo-LfText $New))
	Write-SourceText -Path $Path -LfText $text -Crlf $f.Crlf
	Write-Host "HmRdp tuning patch applied to $(Split-Path -Leaf $Path)"
}

# Tabs <n> <line> builds one C line with <n> tab indents; the line is passed as a
# literal (single-quoted) string, so C quotes need no PowerShell escaping.
function Tabs([int]$count, [string]$line) {
	return ("`t" * $count) + $line
}

# Regular-expression variant of Patch-Block, for the steps whose anchor has to
# absorb whatever version of the region is in the tree (see the step comments).
# By default only the first match is replaced; -All replaces every match.
function Patch-Regex {
	param([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker, [switch]$All)
	$f = Read-SourceText -Path $Path
	if ($Marker -and $f.Text.Contains((ConvertTo-LfText $Marker))) {
		Write-Host "HmRdp progressive tuning already applied to $(Split-Path -Leaf $Path)"
		return
	}
	$re = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
	if (-not $re.IsMatch($f.Text)) {
		throw "HmRdp progressive tuning: pattern not found in $Path"
	}
	$rep = ConvertTo-LfText $Replacement
	$evaluator = [System.Text.RegularExpressions.MatchEvaluator] { param($m) $rep }
	if ($All) {
		$text = $re.Replace($f.Text, $evaluator)
	} else {
		$text = $re.Replace($f.Text, $evaluator, 1)
	}
	Write-SourceText -Path $Path -LfText $text -Crlf $f.Crlf
	Write-Host "HmRdp progressive tuning applied to $(Split-Path -Leaf $Path)"
}

function Patch-Regex-All {
	param([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker)
	Patch-Regex -Path $Path -Pattern $Pattern -Replacement $Replacement -Marker $Marker -All
}

# Copies one of the data files under native/patches/ over the tree, keeping the
# destination file's own line-ending style.
function Copy-PatchData {
	param([string]$From, [string]$To)
	if (-not (Test-Path -LiteralPath $From)) {
		throw "patch source not found: $From"
	}
	$crlf = $false
	if (Test-Path -LiteralPath $To) {
		$crlf = [System.IO.File]::ReadAllText($To).Contains("`r`n")
	}
	$content = ConvertTo-LfText ([System.IO.File]::ReadAllText($From))
	Write-SourceText -Path $To -LfText $content -Crlf $crlf
}

# Apply every step in name order. Each one reports itself ("applied" / "already
# applied"), so a run over an already patched tree should print one line per step
# and nothing else. A failure names the step it came from (the helpers above are
# shared, so the throw site alone would not).
$steps = Get-ChildItem -LiteralPath "$PSScriptRoot\patch-steps" -Filter *.ps1 | Sort-Object Name
foreach ($step in $steps) {
	if ($Trace) {
		Write-Host "== $($step.Name)"
	}
	try {
		. $step.FullName
	} catch {
		throw "patch step $($step.Name) failed: $($_.Exception.Message)"
	}
}

Write-Host "FreeRDP OHOS patches applied to $Source"
