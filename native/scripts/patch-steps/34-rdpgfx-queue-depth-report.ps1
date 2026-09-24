# 34) HmRdp: report the client's real GFX backlog in the frame acknowledge.
#
#     The app moves the frame pipeline off the drdynvc thread and buffers the
#     channel data (see patch 33 / HmrdpSetGfxDataSink). With the frame-rate cap
#     on, that queue grows unless the server is told to slow down - and the
#     server only throttles on the acknowledge's queueDepth (MS-RDPEGFX 3.2.5.13),
#     which upstream always sends as QUEUE_DEPTH_UNAVAILABLE (0 = "no info").
#
#     queueDepth is defined as the bytes of graphics data buffered at the client
#     and not yet processed - exactly the app's queue - so reporting it honestly
#     is what bounds the queue and makes the cap a real, server-side throttle,
#     with no synthesised value. 0 keeps upstream's behaviour.
$gfxC = "$Source\channels\rdpgfx\client\rdpgfx_main.c"

$reportHook = @(
  '/* ---- HmRdp: GFX backlog report ------------------------------------------- */'
  '/* Acknowledge queueDepth = bytes of graphics data buffered at the client and'
  ' * not yet processed, i.e. the app''s GFX offload queue. Reporting it lets the'
  ' * server throttle the frame rate to what the client actually drains instead of'
  ' * the client buffering without bound. */'
  'static UINT32 (*g_HmrdpGfxQueueDepthFn)(void) = NULL;'
  ''
  'FREERDP_API void HmrdpSetGfxQueueDepthFn(UINT32 (*fn)(void))'
  '{'
  "`tg_HmrdpGfxQueueDepthFn = fn;"
  '}'
  ''
) -join "`r`n"
$endFrameAnchor = 'static UINT rdpgfx_recv_end_frame_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)'
Patch-Block $gfxC $endFrameAnchor ($reportHook + $endFrameAnchor) 'HmrdpSetGfxQueueDepthFn('

$ackOld = @(
  (Tabs 2 'else')
  (Tabs 2 '{')
  (Tabs 3 'ack.queueDepth = QUEUE_DEPTH_UNAVAILABLE;')
  ''
  (Tabs 3 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 4 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 4 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "", error);')
  (Tabs 2 '}')
) -join "`r`n"
$ackNew = @(
  (Tabs 2 'else')
  (Tabs 2 '{')
  (Tabs 3 'ack.queueDepth = QUEUE_DEPTH_UNAVAILABLE;')
  ''
  (Tabs 3 '/* HmRdp: report the app''s real backlog so the server throttles to the rate')
  (Tabs 3 ' * the client drains. 0 (no backlog) keeps QUEUE_DEPTH_UNAVAILABLE. */')
  (Tabs 3 'if (g_HmrdpGfxQueueDepthFn != NULL)')
  (Tabs 3 '{')
  (Tabs 4 'const UINT32 backlog = g_HmrdpGfxQueueDepthFn();')
  ''
  (Tabs 4 'if (backlog > 0)')
  (Tabs 5 'ack.queueDepth = (backlog < SUSPEND_FRAME_ACKNOWLEDGEMENT)')
  (Tabs 6 '                     ? backlog')
  (Tabs 6 '                     : (SUSPEND_FRAME_ACKNOWLEDGEMENT - 1);')
  (Tabs 3 '}')
  ''
  (Tabs 3 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 4 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 4 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "", error);')
  (Tabs 2 '}')
) -join "`r`n"
Patch-Block $gfxC $ackOld $ackNew 'g_HmrdpGfxQueueDepthFn != NULL'
