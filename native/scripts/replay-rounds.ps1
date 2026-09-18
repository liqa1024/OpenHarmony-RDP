<#
.SYNOPSIS
  Standard multi-round replay measurement on the dev page.

.DESCRIPTION
  The page reports its state first in every stats text:

      state=running|finished|aborted  run=<n>  route=...  ...

  so a round can be driven deterministically:

    * a click is only sent once the previous round has reached
      state=finished/aborted (clicking a disabled-mid-run control would cut that
      round short, and an aborted round is not a measurement);
    * the wait is for *this* round's number - the figures of two rounds look
      identical otherwise, which is exactly how a measurement gets attributed to
      the wrong round;
    * `state=aborted` is reported, not silently treated as a result.

  Per round the script:
    1. sets the requested modes (参考/节拍/路线) by clicking the control
       until its label matches; each intermediate click starts a short run, and
       the script waits for it to finish before the next click;
    2. optionally pushes the capture and/or the golden reference into the app's
       files dir (the reference is passed by tag; the local copies live in
       .cache/hmrdp_ref_<tag>.{hash,bmp});
    3. clicks 重新回放, waits for that run to finish, and appends the full stats
       text to the output file;
    4. prints a one-line summary per round.

.EXAMPLE
  native/scripts/replay-rounds.ps1 -Capture .cache/hmrdp_gfx_video.bin `
      -RefTag fefd78fd -Rounds "参考:关","参考:对比","参考:关","参考:对比" `
      -Out .cache/rounds-video.txt
#>
param(
  [string]$Device = "",
  [string[]]$Rounds = @("参考:关"),
  [string]$Capture = "",
  [string]$RefTag = "",
  [string]$Out = "rounds.txt",
  [int]$WaitSeconds = 240
)

$ErrorActionPreference = "Stop"

# --- device / app files dir -------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Device)) {
  $targets = @(& hdc list targets 2>$null | Where-Object { $_ -match "\S" -and $_ -notmatch "\[Empty\]" })
  $physical = @($targets | Where-Object { $_ -notmatch "^127\.0\.0\.1:" -and $_ -notmatch "^emulator-" })
  if ($physical.Count -ne 1) { throw "need exactly one physical device (or pass -Device)" }
  $Device = ($physical[0] -split "\s+")[0]
}
$Files = "/data/app/el2/100/base/com.lixa.hmrdp/haps/entry/files"
$LayoutRemote = "/data/local/tmp/replay_layout.json"
$LayoutLocal = Join-Path $env:TEMP "replay_layout.json"

function Get-Layout {
  & hdc -t $Device shell "uitest dumpLayout -p $LayoutRemote" | Out-Null
  & hdc -t $Device file recv $LayoutRemote $LayoutLocal | Out-Null
  return (Get-Content -Raw $LayoutLocal | ConvertFrom-Json)
}

# All text nodes, plus the bounds of the ones that are clickable controls.
function Read-Screen {
  $j = Get-Layout
  $texts = New-Object System.Collections.ArrayList
  $controls = @{}
  function Walk($n) {
    $t = [string]$n.attributes.text
    if ($t -ne "") {
      [void]$texts.Add($t)
      if ($n.attributes.clickable -eq "true" -and -not $controls.ContainsKey($t)) {
        $controls[$t] = $n.attributes.bounds
      }
    }
    foreach ($c in $n.children) { Walk $c }
  }
  Walk $j
  return @{ texts = $texts; controls = $controls }
}

function Get-RunState {
  param($Screen)
  foreach ($t in $Screen.texts) {
    if ($t -match "^state=(\w+)\s+run=(\d+)") { return @{ state = $Matches[1]; run = [int]$Matches[2] } }
  }
  return $null
}

function Wait-RunEnd {
  param([int]$After = -1, [string]$Why = "")
  $deadline = (Get-Date).AddSeconds($WaitSeconds)
  while ((Get-Date) -lt $deadline) {
    $s = Get-RunState (Read-Screen)
    if ($null -ne $s -and $s.state -ne "running" -and $s.run -gt $After) { return $s }
    Start-Sleep -Seconds 2
  }
  throw "timeout waiting for a finished run ($Why)"
}

function Get-Center {
  param([string]$Bounds)
  if ($Bounds -match "^\[(\d+),(\d+)\]\[(\d+),(\d+)\]$") {
    # Arithmetic, not string concatenation: PowerShell's `+` glues strings
    # together, so "2793" + "2952" would become 27932952 and the click would go
    # somewhere far off-screen (silently - no click, no error).
    $x = [int]([int]$Matches[1] + [int]$Matches[3]) / 2
    $y = [int]([int]$Matches[2] + [int]$Matches[4]) / 2
    return @([int]$x, [int]$y)
  }
  throw "cannot parse bounds: $Bounds"
}

# Clicks a control and does not wait for a run (navigation only).
function Click-Plain {
  param([string]$Label)
  $screen = Read-Screen
  if (-not $screen.controls.ContainsKey($Label)) {
    $known = ($screen.controls.Keys -join ", ")
    throw "control not found: '$Label' (clickable: $known)"
  }
  $c = Get-Center $screen.controls[$Label]
  & hdc -t $Device shell "uitest uiInput click $($c[0]) $($c[1])" | Out-Null
}

# Clicks the control labelled `Label` and waits for the run that click started.
function Click-Control {
  param([string]$Label)
  $screen = Read-Screen
  $before = Get-RunState $screen
  if (-not $screen.controls.ContainsKey($Label)) {
    $known = ($screen.controls.Keys -join ", ")
    throw "control not found: '$Label' (clickable: $known)"
  }
  $c = Get-Center $screen.controls[$Label]
  & hdc -t $Device shell "uitest uiInput click $($c[0]) $($c[1])" | Out-Null
  Start-Sleep -Milliseconds 800
  $beforeRun = $(if ($null -eq $before) { -1 } else { $before.run })
  # A click that did not register (window not in front, control disabled, wrong
  # coordinates) looks exactly like a slow run from here, so say so right away
  # instead of sitting in the long wait below.
  $deadline = (Get-Date).AddSeconds(15)
  while ((Get-Date) -lt $deadline) {
    $after = Get-RunState (Read-Screen)
    if ($null -ne $after -and $after.run -gt $beforeRun) {
      Write-Host "    click $Label at ($($c[0]),$($c[1])) -> $($after.state) run=$($after.run)"
      return (Wait-RunEnd -After $beforeRun -Why "after clicking $Label")
    }
    Start-Sleep -Seconds 1
  }
  throw "clicking '$Label' at ($($c[0]),$($c[1])) did not start a new run (still run=$beforeRun)"
}

# Cycles a control (参考/节拍/路线) until its label reads `Target`. Only the
# controls whose value is a state (not a radio row) are here; the decode width
# and its tile decomposition are not settings any more.
$CYCLES = @{
  "参考" = @("关", "导出", "对比")
  "节拍" = @("跑满", "实时")
  "路线" = @("CPU", "硬件加速")
}

function Set-Mode {
  param([string]$Control, [string]$Target)
  $order = $CYCLES[$Control]
  for ($i = 0; $i -lt 8; $i++) {
    $screen = Read-Screen
    $label = $null
    foreach ($t in $screen.texts) { if ($t -match "^$Control(:|：)") { $label = $t; break } }
    if ($null -eq $label) { throw "label for $Control not found" }
    $current = ($label -split "[:：]")[1]
    if ($current -eq $Target) { return }
    if ($script:dryRun) { throw "would click $label" }
    Click-Control -Label $label | Out-Null
  }
  throw "could not set $Control to $Target"
}

function Push-File {
  param([string]$Local, [string]$Name)
  if (-not (Test-Path -LiteralPath $Local)) { throw "local file not found: $Local" }
  & hdc -t $Device file send $Local "$Files/$Name" | Out-Null
  Write-Host "  pushed $Name ($((Get-Item -LiteralPath $Local).Length) bytes)"
}

# --- rounds -----------------------------------------------------------------
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
"" | Set-Content -LiteralPath $outPath -Encoding utf8

# Bring the app to the front and make sure the dev page is the visible one:
# `uitest uiInput click` goes to whatever window is on top, so a restored window
# that is not in front would silently swallow every click.
& hdc -t $Device shell "aa start -a EntryAbility -b com.lixa.hmrdp" | Out-Null
Start-Sleep -Seconds 4
$screen = Read-Screen
if (-not $screen.controls.ContainsKey("重新回放")) {
  if ($screen.controls.ContainsKey("回放测试")) {
    Click-Plain -Label "回放测试"
    Start-Sleep -Seconds 3
  } else {
    throw "the replay page is not reachable (bring it up and retry)"
  }
}

# Make sure no round is running.
$s = Wait-RunEnd -After -1 -Why "initial"
Write-Host "page is up (state=$($s.state) run=$($s.run)); writing $outPath"

$index = 0
foreach ($round in $Rounds) {
  $index++
  Write-Host "== round ${index}: $round =="
  foreach ($setting in ($round -split "[,;]")) {
    if ($setting -match "^\s*$") { continue }
    $kv = $setting.Trim() -split "[:：]"
    if ($kv.Count -ne 2) { throw "bad setting '$setting' (expected e.g. 参考:对比)" }
    Set-Mode -Control $kv[0] -Target $kv[1]
  }
  if ($Capture -ne "") {
    $capPath = if ([System.IO.Path]::IsPathRooted($Capture)) { $Capture } else { Join-Path $repo $Capture }
    Push-File -Local $capPath -Name "hmrdp_gfx.bin"
  }
  if ($RefTag -ne "") {
    Push-File -Local (Join-Path $repo ".cache/hmrdp_ref_$RefTag.hash") -Name "hmrdp_ref_$RefTag.hash"
    Push-File -Local (Join-Path $repo ".cache/hmrdp_ref_$RefTag.bmp") -Name "hmrdp_ref_$RefTag.bmp"
  }

  $screen = Read-Screen
  $started = Get-RunState $screen
  Click-Control -Label "重新回放" | Out-Null
  $end = Wait-RunEnd -After $(if ($null -eq $started) { -1 } else { $started.run }) -Why "round $index"

  $screen = Read-Screen
  $stats = ($screen.texts | Where-Object { $_ -match "^state=" } | Select-Object -First 1)
  $head = "## round ${index}: $round  [$($end.state) run=$($end.run)]"
  $head | Add-Content -LiteralPath $outPath -Encoding utf8
  $stats | Add-Content -LiteralPath $outPath -Encoding utf8
  ($screen.texts | Where-Object { $_ -match "^(perFrame|prog |prog2|par  |dwt check|ref compare|setup ms|upload rects|run  threads|energy:)" }) |
    ForEach-Object { $_ | Add-Content -LiteralPath $outPath -Encoding utf8 }
  "" | Add-Content -LiteralPath $outPath -Encoding utf8

  $one = ($stats -split "`n")[0]
  Write-Host "  $one"
  if ($end.state -ne "finished") { Write-Host "  WARNING: run was $($end.state) - not a measurement" }
}

Write-Host "done: $outPath"
