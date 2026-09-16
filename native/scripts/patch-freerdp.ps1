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
#    a caller-owned rdpContext, which the CPU/gdi replay route uses (see
#    doc_agent/gfx-engine.md §6). NOTE: applied as one block - a tree with the old step 7 must be
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

# 8) Progressive: the per-(tile,component) predictor state must start defined.
#    `progressive_tile_new` fills `tile->data` with 0xFF but takes `sign`/`current`
#    straight from malloc, and a DIFFERENCE (or UPGRADE) pass *reads* them before
#    writing. The encoder assumes zero initial state, so leaving the heap contents
#    there makes the reference decoder's output depend on unrelated allocations -
#    which is exactly a divergence against an engine that zero-initialises its own
#    equivalent buffers (and it is not reproducible across processes).
Patch-File "$Source\libfreerdp\codec\progressive.c" @{
  @'
	size_t signLen = (8192ULL + 32ULL) * 3ULL;
	tile->sign = (BYTE*)winpr_aligned_malloc(signLen, 16);
	if (!tile->sign)
		goto fail;

	size_t currentLen = (8192ULL + 32ULL) * 3ULL;
	tile->current = (BYTE*)winpr_aligned_malloc(currentLen, 16);
	if (!tile->current)
		goto fail;
'@ = @'
	size_t signLen = (8192ULL + 32ULL) * 3ULL;
	tile->sign = (BYTE*)winpr_aligned_malloc(signLen, 16);
	if (!tile->sign)
		goto fail;
	memset(tile->sign, 0, signLen);

	size_t currentLen = (8192ULL + 32ULL) * 3ULL;
	tile->current = (BYTE*)winpr_aligned_malloc(currentLen, 16);
	if (!tile->current)
		goto fail;
	memset(tile->current, 0, currentLen);
'@
}

# 9) HmRdp: client-side bandwidth / frame-loop / thread-fanout tuning.
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
#    (c) winpr pool workers + the drdynvc thread: call an app-registered
#        per-thread QoS hook (HarmonyOS QoS levels - a frame's producer and its
#        tile-decoding consumers want the same treatment), and cap the pool
#        fan-out: one worker per core, woken for every Progressive message,
#        costs power without buying throughput.
function Patch-Block {
  param([string]$Path, [string]$Old, [string]$New, [string]$Marker)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "file not found: $Path"
  }
  $text = [System.IO.File]::ReadAllText($Path)
  if ($Marker -and $text.Contains($Marker)) {
    Write-Host "HmRdp tuning patch already applied to $(Split-Path -Leaf $Path)"
    return
  }
  if (-not $text.Contains($Old)) {
    throw "HmRdp tuning patch: block not found in $Path"
  }
  $text = $text.Replace($Old, $New)
  [System.IO.File]::WriteAllText($Path, $text)
  Write-Host "HmRdp tuning patch applied to $(Split-Path -Leaf $Path)"
}

# (a) receive window: deliberately NOT patched. Asking for a larger SO_RCVBUF
#     explicitly disables the kernel's receive-buffer auto-tuning; on the test
#     device that dropped the measured arrival rate from ~1.28 MB/s to ~0.58 MB/s
#     (same content and same client work, RTT unchanged/better), i.e. upstream's
#     "at least 32 K plus auto-tuning" is the better behaviour here. The receive
#     window is still what caps a high-RTT link, but it cannot be fixed from the
#     client side on this platform.

# Tabs <n> <line> builds one C line with <n> tab indents; the line is passed as a
# literal (single-quoted) string, so C quotes need no PowerShell escaping.
function Tabs([int]$count, [string]$line) {
  return ("`t" * $count) + $line
}

# Tabs <n> <line> builds one C line with <n> tab indents; the line is passed as a
# literal (single-quoted) string, so C quotes need no PowerShell escaping.
function Tabs([int]$count, [string]$line) {
  return ("`t" * $count) + $line
}

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
$poolInitOld = @(
  "`tSYSTEM_INFO info = { 0 };",
  "`tGetSystemInfo(&info);",
  "`tif (info.dwNumberOfProcessors < 1)",
  "`t`tinfo.dwNumberOfProcessors = 1;",
  "`tif (!SetThreadpoolThreadMinimum(pool, info.dwNumberOfProcessors))",
  "`t`tgoto fail;",
  "`tSetThreadpoolThreadMaximum(pool, info.dwNumberOfProcessors);"
) -join "`r`n"
$poolInitNew = @(
  "`tSYSTEM_INFO info = { 0 };",
  "`tGetSystemInfo(&info);",
  "`tif (info.dwNumberOfProcessors < 1)",
  "`t`tinfo.dwNumberOfProcessors = 1;",
  "`t/* HmRdp: cap the per-pool fan-out. FreeRDP submits one work item per",
  "`t * Progressive tile and this class of device has many cores; keeping (and",
  "`t * waking) one worker per core for every message costs power without buying",
  "`t * throughput, so the pool is bounded to a few workers. */",
  "`tif (info.dwNumberOfProcessors > 4)",
  "`t`tinfo.dwNumberOfProcessors = 4;",
  "`tif (!SetThreadpoolThreadMinimum(pool, info.dwNumberOfProcessors))",
  "`t`tgoto fail;",
  "`tSetThreadpoolThreadMaximum(pool, info.dwNumberOfProcessors);"
) -join "`r`n"
Patch-Block $poolC $poolInitOld $poolInitNew 'HmRdp: cap the per-pool fan-out'

$qosHook = @(
  "/* ---- HmRdp: per-thread QoS hook ---------------------------------------- */",
  "/* The application registers a callback (libhmrdp dlopen()s libqos.so) that",
  " * raises the QoS level of the calling thread. Every pool worker and the",
  " * drdynvc thread call it once: a frame's tile decoding is spread over these",
  " * workers, and the platform schedules a marked thread with less wake-up and",
  " * preemption latency (HarmonyOS ""QoS 开发指导"" - mark both halves of a",
  " * producer/consumer pair). Nothing happens when no app registered a hook. */",
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
Patch-Block $poolC "`tpool = (PTP_POOL)arg;" `
  ("`tpool = (PTP_POOL)arg;`r`n`tHmrdpApplyThreadQoS();") 'HmrdpApplyThreadQoS();'

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

Write-Host "FreeRDP OHOS patches applied to $Source"
