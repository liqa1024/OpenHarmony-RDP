# Applies the HarmonyOS/musl compatibility patches to a FreeRDP source tree.
#
# Usage:  pwsh -File native/scripts/patch-freerdp.ps1 [-Source <path-to-FreeRDP>]
#
# The build pipeline expects the patched tree under native/third_party/FreeRDP
# (see native/scripts/build-freerdp.ps1). Run this script once after cloning
# FreeRDP 3.10.3 and before configuring the CMake build.
param(
  [string]$Source = "$PSScriptRoot\..\third_party\FreeRDP"
)

$ErrorActionPreference = "Stop"

function Patch-File {
  param([string]$Path, [hashtable]$Replacements)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "file not found: $Path"
  }
  $text = [System.IO.File]::ReadAllText($Path)
  foreach ($key in $Replacements.Keys) {
    $text = $text.Replace($key, $Replacements[$key])
  }
  [System.IO.File]::WriteAllText($Path, $text)
}

# 1) musl/OHOS has no pthread_cancel.
Patch-File "$Source\winpr\libwinpr\thread\thread.c" @{
  '#ifndef ANDROID' = '#if !defined(ANDROID) && !defined(__OHOS__)'
}

# 2) Force client/common to be built as a shared library (default is static and
#    would not export the client-common symbols the NAPI wrapper needs).
Patch-File "$Source\client\common\CMakeLists.txt" @{
  'addtargetwithresourcefile(${MODULE_NAME} FALSE' = 'addtargetwithresourcefile(${MODULE_NAME} SHARED'
}

# 3) OpenSLES: OHOS ships the standard OpenSLES 1.0.1 headers, not the Android
#    extensions, so map the Android-only identifiers to the standard ones.
$sl = @{
  '#include <SLES/OpenSLES_Android.h>'               = '#include <SLES/OpenSLES.h>'
  '#include <SLES/OpenSLES_AndroidConfiguration.h>'  = '#include <SLES/OpenSLES.h>'
  'SL_IID_ANDROIDSIMPLEBUFFERQUEUE'                  = 'SL_IID_BUFFERQUEUE'
  'SLAndroidSimpleBufferQueueItf'                    = 'SLBufferQueueItf'
  'SLDataLocator_AndroidSimpleBufferQueue'           = 'SLDataLocator_BufferQueue'
  'SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE'          = 'SL_DATALOCATOR_BUFFERQUEUE'
}
foreach ($file in @(
    "$Source\channels\rdpsnd\client\opensles\opensl_io.c",
    "$Source\channels\rdpsnd\client\opensles\opensl_io.h",
    "$Source\channels\audin\client\opensles\opensl_io.c",
    "$Source\channels\audin\client\opensles\opensl_io.h"
  )) {
  Patch-File $file $sl
}

# 4) Replace the rdpsnd OpenSL ES backend with the HmRdp one. OHOS only
#    implements the deprecated OpenSL ES shim and its audio device open proved
#    fragile, so the backend no longer opens a device itself: it decodes to
#    16-bit PCM and hands it to the sink registered by libhmrdp
#    (HmrdpSetAudioSink), which plays it with ArkTS AudioRenderer.
#
#    opensl_io.{c,h} stay in the channel's CMake source list, so they are still
#    patched to the OH-compatible identifiers (step 3 + the copies below) even
#    though the backend no longer uses them.
$Patches = "$PSScriptRoot\..\patches"
foreach ($pair in @(
    @("rdpsnd_opensles.c", "$Source\channels\rdpsnd\client\opensles\rdpsnd_opensles.c"),
    @("rdpsnd_opensl_io.c", "$Source\channels\rdpsnd\client\opensles\opensl_io.c"),
    @("rdpsnd_opensl_io.h", "$Source\channels\rdpsnd\client\opensles\opensl_io.h")
  )) {
  $from = Join-Path $Patches $pair[0]
  if (-not (Test-Path -LiteralPath $from)) {
    throw "patch source not found: $from"
  }
  Copy-Item -LiteralPath $from -Destination $pair[1] -Force
}

# 5) Build unversioned libX.so libraries (no libX.so.3 suffix). Replaces the
#    upstream AddTargetWithResourceFile.cmake so that, with
#    -DWITH_LIBRARY_VERSIONING=OFF (see build-freerdp.ps1), the non-versioning
#    branch keeps the "lib" prefix and emits an explicit SONAME on OHOS/Linux.
Copy-Item -LiteralPath (Join-Path $Patches "AddTargetWithResourceFile.cmake") `
  -Destination "$Source\cmake\AddTargetWithResourceFile.cmake" -Force

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

# 7) HmRdp GFX raw capture + offline replay hooks. The raw (still ZGFX-compressed)
#    channel bytes the server sent are handed to libhmrdp through a runtime
#    callback registered by libhmrdp (a DVC plugin loads before the NAPI module,
#    so a weak symbol in libfreerdp would not resolve). A small replay entry
#    re-runs FreeRDP's own ZGFX + PDU parsing offline for the same stream;
#    HmrdpGfxReplayNewWithContext/FreeWithContext additionally bind the plugin to
#    a caller-owned rdpContext, which the CPU/gdi replay route uses (PERF-TODO
#    §4.2). NOTE: applied as one block - a tree with the old step 7 must be
#    re-patched from a clean source, not incrementally.
$rdpgfxMain = "$Source\channels\rdpgfx\client\rdpgfx_main.c"
if (-not (Test-Path -LiteralPath $rdpgfxMain)) {
  throw "file not found: $rdpgfxMain"
}
if ([System.IO.File]::ReadAllText($rdpgfxMain).Contains('HmrdpSetGfxRawCapture')) {
  Write-Host "HmRdp GFX capture/replay patch already applied"
} else {
  $gfxHelpers = @'
/* ---- HmRdp: raw GFX capture + offline replay hooks ---------------------- */
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
/* ---- HmRdp offline replay ----------------------------------------------- */

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

/* Same as HmrdpGfxReplayNew(), but binds the plugin to a caller-owned
 * rdpContext (libhmrdp's offline gdi context) instead of allocating a bare
 * settings-only one, so gfx->rdpcontext carries the real settings/update.
 * The caller keeps ownership of rcontext; free the context with
 * HmrdpGfxReplayFreeWithContext(). */
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
  $text = [System.IO.File]::ReadAllText($rdpgfxMain)
  $text = $text.Replace('static UINT rdpgfx_recv_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)',
                        $gfxHelpers + 'static UINT rdpgfx_recv_pdu(GENERIC_CHANNEL_CALLBACK* callback, wStream* s)')
  $text = $text.Replace("`tstatus = zgfx_decompress(gfx->zgfx, Stream_ConstPointer(data),",
                        "`tstatus = HmrdpGfxRawCapture(gfx->zgfx, Stream_ConstPointer(data),")
  $text = $text.Replace('FREERDP_ENTRY_POINT(UINT VCAPITYPE rdpgfx_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))',
                        $gfxReplay + 'FREERDP_ENTRY_POINT(UINT VCAPITYPE rdpgfx_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* pEntryPoints))')
  [System.IO.File]::WriteAllText($rdpgfxMain, $text)
  Write-Host "HmRdp GFX capture/replay patch applied"
}

# 8) HmRdp dev-only Progressive state getter (correctness debugging).
#    The GPU engine keeps its own per-(tile,component) Progressive state
#    (`cur`/`sign`/`bitPos`), and the only way to bisect a decode divergence
#    against FreeRDP is to read FreeRDP's internal equivalent
#    (`RFX_PROGRESSIVE_TILE::current/sign/yBitPos` + the 64x64 BGRA `data`).
#    Both structs are private to libfreerdp/codec/progressive.h, so the getter
#    lives inside progressive.c and is exported for the replay harness
#    (declared weak there, so an unpatched tree still links).
#    Dev/verification only: never called by the product code paths.
$progressiveMain = "$Source\libfreerdp\codec\progressive.c"
if (-not (Test-Path -LiteralPath $progressiveMain)) {
  throw "file not found: $progressiveMain"
}
if ([System.IO.File]::ReadAllText($progressiveMain).Contains('HmrdpProgressiveTileState')) {
  Write-Host "HmRdp progressive tile-state patch already applied"
} else {
  $progressiveHook = @'

/* ---- HmRdp: dev-only Progressive tile state getter ---------------------- */
/* Exposes one RFX_PROGRESSIVE_TILE's decoder state so the replay harness can
 * A/B the GPU engine's own `cur`/`sign`/`bitPos` buffers against FreeRDP's.
 * `bitPos` is emitted in the engine's band order
 * [HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3] (the shader's kSubOff order).
 * Returns 1 on success, -1 when the surface/tile is unknown. */
typedef struct
{
	const INT16* current; /* 4096 accumulated coefficients (pre-DWT) */
	const INT16* sign;    /* 4096 sign values */
	BYTE bitPos[10];      /* [HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3] */
	const BYTE* data;     /* 64x64 BGRA tile (the composite source) */
	UINT32 stride;
	UINT32 gridWidth;
	UINT32 gridHeight;
	INT16 width;
	INT16 height;
} HMRDP_PROGRESSIVE_TILE_STATE;

FREERDP_API INT32 HmrdpProgressiveTileState(PROGRESSIVE_CONTEXT* progressive, UINT16 surfaceId,
                                            UINT16 xIdx, UINT16 yIdx, UINT16 component,
                                            HMRDP_PROGRESSIVE_TILE_STATE* out)
{
	PROGRESSIVE_SURFACE_CONTEXT* surface = NULL;
	RFX_PROGRESSIVE_TILE* tile = NULL;
	const RFX_COMPONENT_CODEC_QUANT* pos = NULL;
	size_t zIdx = 0;

	if (!progressive || !out || (component > 2))
		return -1;

	surface = progressive_get_surface_data(progressive, surfaceId);
	if (!surface || !surface->tiles)
		return -1;

	if ((xIdx >= surface->gridWidth) || (yIdx >= surface->gridHeight))
		return -1;

	zIdx = ((size_t)yIdx * surface->gridWidth) + xIdx;
	if (zIdx >= surface->tilesSize)
		return -1;

	tile = surface->tiles[zIdx];
	if (!tile)
		return -1;

	out->current = (const INT16*)(&tile->current[((8192 + 32) * component) + 16]);
	out->sign = (const INT16*)(&tile->sign[((8192 + 32) * component) + 16]);
	out->data = tile->data;
	out->stride = tile->stride;
	out->gridWidth = surface->gridWidth;
	out->gridHeight = surface->gridHeight;
	out->width = (INT16)tile->width;
	out->height = (INT16)tile->height;

	switch (component)
	{
		case 0:
			pos = &tile->yBitPos;
			break;
		case 1:
			pos = &tile->cbBitPos;
			break;
		default:
			pos = &tile->crBitPos;
			break;
	}

	out->bitPos[0] = pos->HL1;
	out->bitPos[1] = pos->LH1;
	out->bitPos[2] = pos->HH1;
	out->bitPos[3] = pos->HL2;
	out->bitPos[4] = pos->LH2;
	out->bitPos[5] = pos->HH2;
	out->bitPos[6] = pos->HL3;
	out->bitPos[7] = pos->LH3;
	out->bitPos[8] = pos->HH3;
	out->bitPos[9] = pos->LL3;
	return 1;
}
'@
  $text = [System.IO.File]::ReadAllText($progressiveMain)
  [System.IO.File]::WriteAllText($progressiveMain, $text + $progressiveHook)
  Write-Host "HmRdp progressive tile-state patch applied"
}

Write-Host "FreeRDP OHOS patches applied to $Source"
