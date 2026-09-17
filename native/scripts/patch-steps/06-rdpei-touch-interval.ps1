# 6) RDPEI touch frame rate: upstream coalesces all changed contacts into one
#    frame every ~20ms (50Hz). Expose the interval as a runtime-settable global
#    so libhmrdp can forward touch at the full input rate when the user enables
#    it; the 20ms default keeps the upstream behaviour untouched.
$rdpeiInterval = @'
/* HmRdp: RDPEI touch frame interval in milliseconds (HmrdpSetTouchFrameInterval).
 * Upstream coalesces touch to one frame per ~20ms (50Hz); HmRdp lowers this at
 * runtime so contacts are forwarded at the full input rate. 20 is the default. */
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

