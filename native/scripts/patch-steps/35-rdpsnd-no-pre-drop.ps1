# 35) Make the app sink the only owner of client-side audio buffering: rdpsnd's wall-clock overrun
#     guard (~1 packet here) would drop audio the sink can still play, so neutralise it.
$sndC = "$Source\channels\rdpsnd\client\rdpsnd_main.c"

$old = @(
  'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)'
  '{'
  (Tabs 1 'UINT32 bpf = 0;')
) -join "`r`n"
$new = @(
  'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)'
  '{'
  (Tabs 1 '/* HmRdp: the app sink owns all client-side buffering and reports real latency; never pre-drop here. */')
  (Tabs 1 'return FALSE;')
  ''
  (Tabs 1 'UINT32 bpf = 0;')
) -join "`r`n"
Patch-Block $sndC $old $new 'HmRdp: the app sink owns all client-side buffering'
