# 33) Hand the raw RDPGFX channel chunk to the app so the drdynvc thread is not stalled by a frame
#     (audio/input queue behind it); the app re-enters via rdpgfx_process_data(). No sink = unchanged.
$gfxC = "$Source\channels\rdpgfx\client\rdpgfx_main.c"

Patch-Block $gfxC 'static UINT rdpgfx_on_data_received(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)' `
  'static UINT rdpgfx_process_data(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)' `
  'static UINT rdpgfx_process_data(IWTSVirtualChannelCallback* pChannelCallback, wStream* data)'

$sinkHook = @(
  '/* HmRdp: when set, the app owns the raw GFX chunk (drdynvc returns immediately so audio/input'
  ' * are not held behind the frame); the app re-enters via rdpgfx_process_data(). */'
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

# 33c) The replay entry bypasses the sink: it already runs on the sink's own thread.
Patch-Block $gfxC "`treturn rdpgfx_on_data_received(&callback.iface, s);" `
  "`treturn rdpgfx_process_data(&callback.iface, s);" `
  'return rdpgfx_process_data(&callback.iface, s);'
