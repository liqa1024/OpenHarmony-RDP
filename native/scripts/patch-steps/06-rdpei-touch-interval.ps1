# 6) Make the RDPEI touch coalescing interval (~20ms upstream) runtime-settable; the default keeps upstream behaviour.
$rdpeiInterval = @'
/* HmRdp: RDPEI touch frame interval in ms (HmrdpSetTouchFrameInterval); upstream coalesces at ~20ms. */
UINT32 g_HmrdpTouchFrameIntervalMs = 20;

void HmrdpSetTouchFrameInterval(UINT32 intervalMs)
{
	g_HmrdpTouchFrameIntervalMs = intervalMs;
}

static BOOL rdpei_poll_run_unlocked(rdpContext* context, void* userdata)
'@
$rdpeiMain = "$Source\channels\rdpei\client\rdpei_main.c"
if ([System.IO.File]::ReadAllText($rdpeiMain).Contains('g_HmrdpTouchFrameIntervalMs')) {
  Write-Host "HmRdp RDPEI interval patch already applied"
} else {
  Patch-File $rdpeiMain @{
    'static BOOL rdpei_poll_run_unlocked(rdpContext* context, void* userdata)' = $rdpeiInterval
    'lastPollEventTime < 20ULL' = 'lastPollEventTime < g_HmrdpTouchFrameIntervalMs'
  }
}

