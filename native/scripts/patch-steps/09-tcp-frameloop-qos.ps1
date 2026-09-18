# 9) HmRdp: client-side bandwidth / frame-loop / QoS tuning.
#    (a) tcp.c: size the receive window for the bandwidth-delay product. A
#        remote/relayed session has a high RTT, where throughput is capped by
#        window/RTT, and upstream only guarantees a 32 K receive buffer. Set
#        before connect(), which is where the window scale is negotiated.
#    (b) rdpgfx: acknowledge the frame *before* the client's EndFrame callback
#        runs. That callback composites and presents the frame, and upstream
#        only acknowledges after it returns - so the whole local present latency
#        sits inside the server's per-frame round trip (the acknowledge is what
#        paces the next frame). Decoding is already done at this point, so
#        acknowledging here is honest and the next frame is not held back by our
#        presentation.
#    (c) the drdynvc thread calls an app-registered per-thread QoS hook
#        (HarmonyOS QoS levels) for the whole frame pipeline it carries: ZGX
#        decode, PDU parse, image decode, composite, present and acknowledge.
#        The decode's tile workers get their QoS from the platform queue's task
#        attribute instead (hmrdp_parallel.*), so the WinPR pool no longer
#        participates and its fan-out cap is gone.
# (a) receive window: deliberately NOT patched. Asking for a larger SO_RCVBUF
#     explicitly disables the kernel's receive-buffer auto-tuning; on the test
#     device that dropped the measured arrival rate from ~1.28 MB/s to ~0.58 MB/s
#     (same content and same client work, RTT unchanged/better), i.e. upstream's
#     "at least 32 K plus auto-tuning" is the better behaviour here. The receive
#     window is still what caps a high-RTT link, but it cannot be fixed from the
#     client side on this platform.

$gfxC = "$Source\channels\rdpgfx\client\rdpgfx_main.c"
$gfxAckOld = @(
  (Tabs 1 'const UINT64 end = GetTickCount64();')
  (Tabs 1 'const UINT64 EndFrameTime = end - start;')
  (Tabs 1 'gfx->TotalDecodedFrames++;')
  ''
  (Tabs 1 'if (!gfx->sendFrameAcks)')
  (Tabs 2 'return error;')
  ''
  (Tabs 1 'ack.frameId = pdu.frameId;')
  (Tabs 1 'ack.totalFramesDecoded = gfx->TotalDecodedFrames;')
  ''
  (Tabs 1 'if (gfx->suspendFrameAcks)')
  (Tabs 1 '{')
  (Tabs 2 'ack.queueDepth = SUSPEND_FRAME_ACKNOWLEDGEMENT;')
  ''
  (Tabs 2 'if (gfx->TotalDecodedFrames == 1)')
  (Tabs 3 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 4 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 4 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "",')
  (Tabs 4 '           error);')
  (Tabs 1 '}')
  (Tabs 1 'else')
  (Tabs 1 '{')
  (Tabs 2 'ack.queueDepth = QUEUE_DEPTH_UNAVAILABLE;')
  ''
  (Tabs 2 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 3 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 3 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "", error);')
  (Tabs 1 '}')
) -join "`r`n"
$gfxAckNew = @(
  (Tabs 1 'const UINT64 end = GetTickCount64();')
  (Tabs 1 'const UINT64 EndFrameTime = end - start;')
) -join "`r`n"
Patch-Block $gfxC $gfxAckOld $gfxAckNew 'HmRdp: acknowledge the frame before'

$gfxAckEarly = @(
  (Tabs 1 '/* HmRdp: acknowledge the frame before the client''s EndFrame callback. The')
  (Tabs 1 ' * callback composites and presents the frame, and upstream only sends the')
  (Tabs 1 ' * acknowledge after it returns - which puts the whole local present latency')
  (Tabs 1 ' * inside the server''s per-frame round trip. Decoding is already done here, so')
  (Tabs 1 ' * acknowledging now is honest and our presentation no longer paces the stream. */')
  (Tabs 1 'gfx->TotalDecodedFrames++;')
  ''
  (Tabs 1 'if (gfx->sendFrameAcks)')
  (Tabs 1 '{')
  (Tabs 2 'ack.frameId = pdu.frameId;')
  (Tabs 2 'ack.totalFramesDecoded = gfx->TotalDecodedFrames;')
  ''
  (Tabs 2 'if (gfx->suspendFrameAcks)')
  (Tabs 2 '{')
  (Tabs 3 'ack.queueDepth = SUSPEND_FRAME_ACKNOWLEDGEMENT;')
  ''
  (Tabs 3 'if (gfx->TotalDecodedFrames == 1)')
  (Tabs 4 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 5 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 5 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "",')
  (Tabs 5 '           error);')
  (Tabs 2 '}')
  (Tabs 2 'else')
  (Tabs 2 '{')
  (Tabs 3 'ack.queueDepth = QUEUE_DEPTH_UNAVAILABLE;')
  ''
  (Tabs 3 'if ((error = rdpgfx_send_frame_acknowledge_pdu(context, &ack)))')
  (Tabs 4 'WLog_Print(gfx->log, WLOG_ERROR,')
  (Tabs 4 '           "rdpgfx_send_frame_acknowledge_pdu failed with error %" PRIu32 "", error);')
  (Tabs 2 '}')
  (Tabs 1 '}')
  ''
) -join "`r`n"
Patch-Block $gfxC "`tconst UINT64 start = GetTickCount64();" `
  ($gfxAckEarly + "`r`n`tconst UINT64 start = GetTickCount64();") 'HmRdp: acknowledge the frame before'

$poolC = "$Source\winpr\libwinpr\pool\pool.c"
$qosHook = @(
  "/* ---- HmRdp: per-thread QoS hook ---------------------------------------- */",
  "/* The application registers a callback (libhmrdp dlopen()s libqos.so) that",
  " * raises the QoS level of the calling thread. The drdynvc thread calls it once:",
  " * ZGX decode, PDU parse, image decode, composite, present and the frame",
  " * acknowledge for every frame run on that thread, and the platform schedules a",
  " * marked thread with less wake-up and preemption latency (HarmonyOS ""QoS",
  " * 开发指导"" - mark both halves of a producer/consumer pair). Nothing happens",
  " * when no app registered a hook. */",
  "static void (*g_HmrdpThreadQoS)(void) = NULL;",
  "",
  "WINPR_API void HmrdpSetThreadQoSApplier(void (*fn)(void))",
  "{",
  "`tg_HmrdpThreadQoS = fn;",
  "}",
  "",
  "WINPR_API void HmrdpApplyThreadQoS(void)",
  "{",
  "`tif (g_HmrdpThreadQoS)",
  "`t`tg_HmrdpThreadQoS();",
  "}",
  "",
  ""
) -join "`r`n"
Patch-Block $poolC 'static DWORD WINAPI thread_pool_work_func(LPVOID arg)' `
  ($qosHook + 'static DWORD WINAPI thread_pool_work_func(LPVOID arg)') `
  'HmRdp: per-thread QoS hook'

$dvcC = "$Source\channels\drdynvc\client\drdynvc_main.c"
Patch-Block $dvcC "`tdrdynvcPlugin* drdynvc = (drdynvcPlugin*)arg;" `
  ("`t/* HmRdp: ZGX decode, PDU parse, image decode, composite, present and the" + "`r`n" +
   "`t * frame acknowledge for every frame run on this thread; let it tell the" + "`r`n" +
   "`t * platform how important that is (HarmonyOS QoS, see the hook in libwinpr). */" + "`r`n" +
   "`tHmrdpApplyThreadQoS();" + "`r`n" +
   "`tdrdynvcPlugin* drdynvc = (drdynvcPlugin*)arg;") 'HmrdpApplyThreadQoS();'
Patch-Block $dvcC "static DWORD WINAPI drdynvc_virtual_channel_client_thread(LPVOID arg)" `
  ("/* Exported by the patched libwinpr (the app registers the applier). */" + "`r`n" +
   "extern void HmrdpApplyThreadQoS(void);" + "`r`n`r`n" +
   "static DWORD WINAPI drdynvc_virtual_channel_client_thread(LPVOID arg)") `
  'extern void HmrdpApplyThreadQoS(void);'

