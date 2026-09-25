# 35) HmRdp: make the app sink the only owner of client-side audio buffering.
#
#     rdpsnd_detect_overrun() estimates the device's backlog from wall clock and
#     drops a whole wave packet once it exceeds 2 packet durations + rdpsnd's
#     latency. Our backend never sets a latency, so the threshold is ~1 packet:
#     the guard drops audio the sink's own (much deeper, bounded) jitter buffer
#     could still have played. It is a second, worse drop policy next to the
#     sink's, and the sink cannot be seen from here.
#
#     The backend hands PCM to that sink and answers Play with the sink's real
#     queued latency (native/patches/rdpsnd_opensles.c), so the Wave Confirm the
#     server receives stays truthful while the sink alone decides when to start,
#     how much to hold and what to drop. Neutralising the guard here removes the
#     duplicate policy; the protocol, the format negotiation and the decoder are
#     untouched.
$sndC = "$Source\channels\rdpsnd\client\rdpsnd_main.c"

$old = @(
  'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)'
  '{'
  (Tabs 1 'UINT32 bpf = 0;')
) -join "`r`n"
$new = @(
  'static BOOL rdpsnd_detect_overrun(rdpsndPlugin* rdpsnd, const AUDIO_FORMAT* format, size_t size)'
  '{'
  (Tabs 1 '/* HmRdp: the app sink owns all client-side buffering and reports the real')
  (Tabs 1 ' * latency; this wall-clock backlog estimate is a second, worse policy that')
  (Tabs 1 ' * would drop packets the sink can still play. Never pre-drop here. */')
  (Tabs 1 'return FALSE;')
  ''
  (Tabs 1 'UINT32 bpf = 0;')
) -join "`r`n"
Patch-Block $sndC $old $new 'HmRdp: the app sink owns all client-side buffering'
