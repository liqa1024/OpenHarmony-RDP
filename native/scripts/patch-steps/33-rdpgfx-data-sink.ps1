# 33) HmRdp: let the application take the RDPGFX channel data off the drdynvc
#     thread.
#
#     All dynamic channels share one drdynvc dispatch thread, and the whole GFX
#     frame pipeline (ZGX decompress, PDU parse, image decode, composite, present)
#     runs inline on it. While a frame is being processed, audio and input DVC
#     data queued behind it cannot be dispatched, so a heavy frame stalls them.
#
#     With a sink registered, rdpgfx_on_data_received hands the raw channel chunk
#     to the app and returns immediately; the app processes it on its own thread
#     and re-enters through rdpgfx_process_data() (the same path the offline
#     replay drives). No sink registered -> behaviour is unchanged.
$gfxC = "$Source\channels\rdpgfx\client\rdpgfx_main.c"

# 33a) The original receive body becomes the re-entrant processing entry.
Patch-Block $gfxC 'static UINT rdpgfx_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)' `
  'static UINT rdpgfx_process_data(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)' `
  'static UINT rdpgfx_process_data(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)'

# 33b) The DVC callback becomes a thin dispatcher that may hand the chunk to the app.
$sinkHook = @(
  '/* ---- HmRdp: GFX data sink ------------------------------------------------ */'
  '/* When set, the app owns the raw GFX channel chunk: it is handed over and the'
  ' * drdynvc thread returns immediately, so audio and input queued on that thread'
  ' * are not held behind the frame pipeline. The app re-enters via'
  ' * rdpgfx_process_data() on its own thread. This is the thread-safe entry for'
  ' * that: HmrdpGfxReplayRecv already drives it. */'
  'static BOOL (*g_HmrdpGfxDataSink)(RdpgfxClientContext* context, const BYTE* data, UINT32 size) ='
  '    NULL;'
  ''
  'FREERDP_API void HmrdpSetGfxDataSink(BOOL (*fn)(RdpgfxClientContext*, const BYTE*, UINT32))'
  '{'
  "`tg_HmrdpGfxDataSink = fn;"
  '}'
  ''
  'static UINT rdpgfx_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)'
  '{'
  "`tGENERIC_CHANNEL_CALLBACK* callback = (GENERIC_CHANNEL_CALLBACK*)pChannelCallback;"
  "`tWINPR_ASSERT(callback);"
  "`tRDPGFX_PLUGIN* gfx = (RDPGFX_PLUGIN*)callback->plugin;"
  "`tWINPR_ASSERT(gfx);"
  ''
  "`tif (g_HmrdpGfxDataSink != NULL)"
  "`t{"
  "`t`tif (g_HmrdpGfxDataSink(gfx->context, Stream_ConstPointer(data),"
  "`t`t                       (UINT32)Stream_GetRemainingLength(data)))"
  "`t`t`treturn CHANNEL_RC_OK;"
  "`t}"
  "`treturn rdpgfx_process_data(pChannelCallback, data);"
  '}'
  ''
) -join "`r`n"
$cbAnchor = 'static const IWTSVirtualChannelCallback rdpgfx_callbacks = { rdpgfx_on_data_received,'
Patch-Block $gfxC $cbAnchor ($sinkHook + $cbAnchor) 'HmrdpSetGfxDataSink('

# 33c) The replay entry must bypass the sink (it runs on the sink's own thread).
Patch-Block $gfxC "`treturn rdpgfx_on_data_received(&callback.iface, s);" `
  "`treturn rdpgfx_process_data(&callback.iface, s);" `
  'return rdpgfx_process_data(&callback.iface, s);'
