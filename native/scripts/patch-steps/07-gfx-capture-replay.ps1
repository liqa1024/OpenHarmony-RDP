# 7) GFX raw capture + offline replay hooks via a runtime callback (a DVC plugin loads
#    before the NAPI module, so a weak symbol would not resolve). Applied as one block.
$rdpgfxMain = "$Source\channels\rdpgfx\client\rdpgfx_main.c"
if (-not (Test-Path -LiteralPath $rdpgfxMain)) {
  throw "file not found: $rdpgfxMain"
}
if ([System.IO.File]::ReadAllText($rdpgfxMain).Contains('HmrdpSetGfxRawCapture')) {
  Write-Host "HmRdp GFX capture/replay patch already applied"
} else {
  $gfxHelpers = @'
static void (*g_HmrdpGfxRawCapture)(const BYTE* data, UINT32 size) = NULL;

FREERDP_API void HmrdpSetGfxRawCapture(void (*fn)(const BYTE* data, UINT32 size))
{
	g_HmrdpGfxRawCapture = fn;
}

static int HmrdpGfxRawCapture(ZGFX_CONTEXT* zgfx, const BYTE* data, UINT32 size, BYTE** pDst,
                              UINT32* pDstSize, int flags)
{
	if (g_HmrdpGfxRawCapture)
		g_HmrdpGfxRawCapture(data, size);
	return zgfx_decompress(zgfx, data, size, pDst, pDstSize, flags);
}

'@
  $gfxReplay = @'

FREERDP_API RdpgfxClientContext* HmrdpGfxReplayNew(void)
{
	RDPGFX_PLUGIN* gfx = NULL;
	rdpContext* rcontext = NULL;
	rdpSettings* settings = NULL;

	rcontext = (rdpContext*)calloc(1, sizeof(rdpContext));
	if (!rcontext)
		return NULL;
	settings = freerdp_settings_new(0);
	if (!settings)
	{
		free(rcontext);
		return NULL;
	}
	rcontext->settings = settings;
	gfx = (RDPGFX_PLUGIN*)calloc(1, sizeof(RDPGFX_PLUGIN));
	if (!gfx)
	{
		freerdp_settings_free(settings);
		free(rcontext);
		return NULL;
	}
	if (init_plugin_cb(&gfx->base, rcontext, settings) != CHANNEL_RC_OK)
	{
		free(gfx);
		freerdp_settings_free(settings);
		free(rcontext);
		return NULL;
	}
	return gfx->context;
}

FREERDP_API void HmrdpGfxReplayFree(RdpgfxClientContext* context)
{
	RDPGFX_PLUGIN* gfx = NULL;
	if (!context)
		return;
	gfx = (RDPGFX_PLUGIN*)context->handle;
	WINPR_ASSERT(gfx);
	rdpgfx_client_context_free(context);
	freerdp_settings_free(gfx->rdpcontext->settings);
	free(gfx->rdpcontext);
	free(gfx);
}

/* Like HmrdpGfxReplayNew() but binds a caller-owned rcontext (caller keeps ownership). */
FREERDP_API RdpgfxClientContext* HmrdpGfxReplayNewWithContext(rdpContext* rcontext)
{
	RDPGFX_PLUGIN* gfx = NULL;

	if (!rcontext || !rcontext->settings)
		return NULL;
	gfx = (RDPGFX_PLUGIN*)calloc(1, sizeof(RDPGFX_PLUGIN));
	if (!gfx)
		return NULL;
	if (init_plugin_cb(&gfx->base, rcontext, rcontext->settings) != CHANNEL_RC_OK)
	{
		free(gfx);
		return NULL;
	}
	return gfx->context;
}

FREERDP_API void HmrdpGfxReplayFreeWithContext(RdpgfxClientContext* context)
{
	RDPGFX_PLUGIN* gfx = NULL;
	if (!context)
		return;
	gfx = (RDPGFX_PLUGIN*)context->handle;
	WINPR_ASSERT(gfx);
	rdpgfx_client_context_free(context);
	free(gfx);
}

FREERDP_API UINT HmrdpGfxReplayRecv(RdpgfxClientContext* context, const BYTE* data, UINT32 size)
{
	RDPGFX_PLUGIN* gfx = NULL;
	GENERIC_CHANNEL_CALLBACK callback = { 0 };
	wStream sbuffer = { 0 };
	wStream* s = NULL;

	if (!context)
		return ERROR_BAD_ARGUMENTS;
	gfx = (RDPGFX_PLUGIN*)context->handle;
	WINPR_ASSERT(gfx);
	callback.plugin = (IWTSPlugin*)&gfx->base;
	s = Stream_StaticConstInit(&sbuffer, data, size);
	if (!s)
		return CHANNEL_RC_NO_MEMORY;
	return rdpgfx_on_data_received(&callback.iface, s);
}

'@
  $anchorRecv = 'static UINT rdpgfx_recv_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)'
  $anchorEntry = 'FREERDP_ENTRY_POINT(UINT VCAPITYPE rdpgfx_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))'
  Patch-Block $rdpgfxMain $anchorRecv ($gfxHelpers + $anchorRecv) ''
  Patch-File $rdpgfxMain @{
    "`tstatus = zgfx_decompress(gfx->zgfx, Stream_ConstPointer(data)," = "`tstatus = HmrdpGfxRawCapture(gfx->zgfx, Stream_ConstPointer(data),"
  }
  Patch-Block $rdpgfxMain $anchorEntry ($gfxReplay + $anchorEntry) ''
  Write-Host "HmRdp GFX capture/replay patch applied"
}

