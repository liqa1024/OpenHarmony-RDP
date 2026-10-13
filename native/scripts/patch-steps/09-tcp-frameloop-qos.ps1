# 9) GFX frame-loop/QoS: ack a frame before the EndFrame callback (present latency would
#    otherwise pace the stream), and raise the drdynvc thread's QoS. SO_RCVBUF is deliberately left unpatched.
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
  (Tabs 1 '/* HmRdp: acknowledge the frame before the client''s EndFrame callback; decoding is already done here. */')
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
  "/* HmRdp: per-thread QoS hook: libhmrdp registers a callback that raises the",
  " * calling thread's HarmonyOS QoS level (the drdynvc thread runs the whole frame pipeline). */",
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
  ("`t/* HmRdp: tell the platform this thread carries the whole frame pipeline (HarmonyOS QoS). */" + "`r`n" +
   "`tHmrdpApplyThreadQoS();" + "`r`n" +
   "`tdrdynvcPlugin* drdynvc = (drdynvcPlugin*)arg;") 'HmrdpApplyThreadQoS();'
Patch-Block $dvcC "static DWORD WINAPI drdynvc_virtual_channel_client_thread(LPVOID arg)" `
  ("extern void HmrdpApplyThreadQoS(void);" + "`r`n`r`n" +
   "static DWORD WINAPI drdynvc_virtual_channel_client_thread(LPVOID arg)") `
  'extern void HmrdpApplyThreadQoS(void);'

