# 31) HmRdp: stop the generic rdpsnd overrun guard from pre-dropping samples
#     when the backend keeps its own bounded buffer.
#
#     rdpsnd_detect_overrun() silently drops a wave sample once the estimated
#     client-side buffer exceeds 2 packet durations + rdpsnd->latency (0 here),
#     i.e. only ~2 packets. Our OpenSL ES backend hands PCM to a sink that keeps
#     a much larger, bounded jitter buffer and drops on its own if the server
#     really runs ahead, so the generic guard only creates gaps the sink could
#     have absorbed.
#
#     The backend exports HmrdpRdpsndBufferLatencyMs() with the buffer it keeps;
#     a positive value means "the device bounds its own buffering -> do not drop
#     upstream". Weak so a build without that backend keeps upstream behaviour.
$sndC = "$Source\channels\rdpsnd\client\rdpsnd_main.c"

$sndExtern = @(
  '/* HmRdp: exported by the OpenSL ES backend; the client-side PCM buffer the sink'
  ' * keeps, in ms. Positive => the sink bounds its own buffering and drops on its'
  ' * own, so the generic overrun guard must not pre-drop samples. */'
  'extern UINT32 HmrdpRdpsndBufferLatencyMs(void) __attribute__((weak));'
  ''
) -join "`r`n"
Patch-Block $sndC 'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)' `
  ($sndExtern + 'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)') `
  'extern UINT32 HmrdpRdpsndBufferLatencyMs(void)'

$sndGuardOld = @(
  (Tabs 1 'UINT32 maxDuration = 0;')
  ''
  (Tabs 1 'if (!rdpsnd || !format)')
  (Tabs 2 'return FALSE;')
) -join "`r`n"
$sndGuardNew = @(
  (Tabs 1 'UINT32 maxDuration = 0;')
  ''
  (Tabs 1 'if (!rdpsnd || !format)')
  (Tabs 2 'return FALSE;')
  ''
  (Tabs 1 '/* HmRdp: a device that bounds its own buffer is the only throttle; dropping')
  (Tabs 1 ' * here would leave gaps it could have played. */')
  (Tabs 1 'if ((HmrdpRdpsndBufferLatencyMs != NULL) && (HmrdpRdpsndBufferLatencyMs() > 0))')
  (Tabs 2 'return FALSE;')
) -join "`r`n"
Patch-Block $sndC $sndGuardOld $sndGuardNew 'HmrdpRdpsndBufferLatencyMs != NULL'
