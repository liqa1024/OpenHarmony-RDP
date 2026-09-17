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

# 8) HmRdp CPU (gdi) progressive-decode tuning.
#
#    背景：CPU 链路（FreeRDP gdi 渲染）的每帧工时几乎全是 progressive 解码，而解码里
#    又有近一半是"每个 tile 的固定开销"，不是像素运算。下面三处改动都**不改变结果**
#    （同样的像素、同样的脏区面积），只把开销拿掉：
#
#      a) tile 任务从"每 tile 一个 WinPR 线程池任务"改成少量分片 + 共享计数器动态领取。
#         WinPR 的线程池不是免费的：CreateThreadpoolWork 一次 calloc、
#         SubmitThreadpoolWork 再一次 calloc + 入队唤醒、WaitForThreadpoolWorkCallbacks
#         每个任务一次 futex 往返（等的是池的全局完成计数）。一整屏 progressive 帧有
#         1~2k 个 tile，这些记账开销盖过了真正解码。分片既不改变并发度（每个 tile 仍由
#         单个回调解码、scratch 状态不变），也不改变结果（tile 是同一表面上互不重叠的
#         64x64 块）。
#      b) update_tiles() 不再为每个访问到的 tile 建一个 REGION16
#         （init/intersect_rect/rects/uninit = 两次分配 + 一次释放），改成"一次取出裁剪
#         矩形表 + 普通矩形求交"；并用 stamp 让同一次 pass 里每个 tile 只访问一次
#         （帧内 tile 列表按"每次解码"累积，被后续消息细化的 tile 会出现多条，原先每条
#         都重复合成同一份最终像素）。裁剪表按 top 有序，所以到不过 tile 的行即可 break。
#      c) keep-destination-alpha 的 32bpp 拷贝从"每像素三个字节"改成"每像素一个掩码
#         32 位字"（保留目标第 4 字节、取源低三字节 —— 与逐字节写法逐字节等价，与字节序
#         无关）。这是 gdi 链路最热的循环。
#
#    另外导出 HmrdpProgStat[8]（每条消息只碰几次，不在 per-tile 路径上）供 app 的回放
#    统计打印 `prog` 行做归因。整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
function Patch-Regex {
  param([string]$Path, [string]$Pattern, [string]$Replacement, [string]$Marker)
  if (-not (Test-Path -LiteralPath $Path)) {
    throw "file not found: $Path"
  }
  $raw = [System.IO.File]::ReadAllText($Path)
  if ($Marker -and $raw.Contains($Marker)) {
    Write-Host "HmRdp progressive tuning already applied to $(Split-Path -Leaf $Path)"
    return
  }
  # Match on LF-normalized text: the replacement blocks below are written with
  # one line-ending style, upstream files are not consistent. The file's own
  # style is restored on write.
  $crlf = $raw.Contains("`r`n")
  $text = $raw.Replace("`r`n", "`n")
  $Replacement = $Replacement.Replace("`r`n", "`n")
  $re = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
  if (-not $re.IsMatch($text)) {
    throw "HmRdp progressive tuning: pattern not found in $Path"
  }
  $evaluator = [System.Text.RegularExpressions.MatchEvaluator] { param($m) $Replacement }
  $text = $re.Replace($text, $evaluator, 1)
  if ($crlf) {
    $text = $text.Replace("`n", "`r`n")
  }
  [System.IO.File]::WriteAllText($Path, $text)
  Write-Host "HmRdp progressive tuning applied to $(Split-Path -Leaf $Path)"
}

$progH = "$Source\libfreerdp\codec\progressive.h"
$progC = "$Source\libfreerdp\codec\progressive.c"
$copyC = "$Source\libfreerdp\primitives\prim_copy.c"

# (a1) per-tile / per-surface update stamps.
Patch-Regex $progH '\tBYTE flags;\n\tBYTE quality;\n\tBOOL dirty;\n' (@'
	BYTE flags;
	BYTE quality;
	BOOL dirty;
	/* HmRdp: stamp of the last update_tiles() pass that composited this tile,
	 * used to visit each tile at most once per pass (the per-frame tile list
	 * accumulates one entry per decode, so a re-decoded tile appears several
	 * times and used to be composited once per occurrence). */
	UINT32 updateStamp;
'@) 'updateStamp;'

Patch-Regex $progH '\tUINT32 numUpdatedTiles;\n\tUINT32\* updatedTileIndices;\n\} PROGRESSIVE_SURFACE_CONTEXT;' (@'
	UINT32 numUpdatedTiles;
	UINT32* updatedTileIndices;
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
} PROGRESSIVE_SURFACE_CONTEXT;
'@) 'monotonic stamp handed out by each update_tiles'

# (a2) dev-only phase counters + a monotonic clock for them.
Patch-Regex $progC '#include <freerdp/config\.h>\n\n#include <winpr/assert\.h>' (@'
#include <freerdp/config.h>

#include <time.h>

#include <winpr/assert.h>
'@) '#include <time.h>'

Patch-Regex $progC '#define TAG FREERDP_TAG\("codec\.progressive"\)\n' (@'
#define TAG FREERDP_TAG("codec.progressive")

/*
 * HmRdp dev instrumentation: nanosecond totals for the serial phases of a
 * progressive decode, read back by the app's replay stats (`prog` line). They
 * are touched a handful of times per message, never per tile, so the probe
 * itself does not show up in the figures it reports.
 *   [0] tile read/parse (serial)      [4] (unused)
 *   [1] work dispatch (serial)        [5] update_tiles calls
 *   [2] work wait+close (serial)      [6] tiles composited
 *   [3] update_tiles (serial)         [7] of [2], the part that really blocked
 */
unsigned long long HmrdpProgStat[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static INLINE unsigned long long hmrdp_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((unsigned long long)ts.tv_sec * 1000000000ull) + (unsigned long long)ts.tv_nsec;
}
'@) 'HmrdpProgStat'

# (a3) chunked, dynamically balanced tile dispatch.
Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/*
 * HmRdp: the tile work is submitted in a small number of chunks instead of one
 * WinPR threadpool work item per tile (see the patch note in
 * native/scripts/patch-freerdp.ps1 step 8).
 */
typedef struct
{
	PROGRESSIVE_TILE_PROCESS_WORK_PARAM* params;
	volatile UINT32* next;
	UINT32 numTiles;
} PROGRESSIVE_TILE_CHUNK_PARAM;

static void CALLBACK progressive_process_tile_chunk_callback(PTP_CALLBACK_INSTANCE instance,
                                                             void* context, PTP_WORK work)
{
	PROGRESSIVE_TILE_CHUNK_PARAM* chunk = (PROGRESSIVE_TILE_CHUNK_PARAM*)context;

	WINPR_ASSERT(chunk);

	for (;;)
	{
		const UINT32 index = __sync_fetch_and_add(chunk->next, 1u);
		if (index >= chunk->numTiles)
			break;

		progressive_process_tiles_tile_work_callback(instance, &chunk->params[index], work);
	}
}

/* Upper bound on the work items a single region may be split into. */
#define HMRDP_TILE_CHUNKS 64

static INLINE SSIZE_T progressive_process_tiles(
'@) 'HMRDP_TILE_CHUNKS'

Patch-Regex $progC '\twhile \(\(Stream_GetRemainingLength\(s\) >= 6\) &&\n\t       \(region->tileDataSize > \(Stream_GetPosition\(s\) - start\)\)\)\n\t\{\n\t\tconst size_t pos = Stream_GetPosition\(s\);\n' (@'
	const unsigned long long ht0 = hmrdp_now_ns();
	while ((Stream_GetRemainingLength(s) >= 6) &&
	       (region->tileDataSize > (Stream_GetPosition(s) - start)))
	{
		const size_t pos = Stream_GetPosition(s);
'@) 'const unsigned long long ht0 = hmrdp_now_ns();'

Patch-Regex $progC '\tend = Stream_GetPosition\(s\);\n\tif \(\(end - start\) != region->tileDataSize\)' (@'
	HmrdpProgStat[0] += hmrdp_now_ns() - ht0; /* tile read/parse (serial) */
	const unsigned long long ht1 = hmrdp_now_ns();

	end = Stream_GetPosition(s);
	if ((end - start) != region->tileDataSize)
'@) 'HmrdpProgStat[0] +='

Patch-Regex $progC '\tfor \(UINT32 idx = 0; idx < region->numTiles; idx\+\+\)\n\t\{\n\t\tRFX_PROGRESSIVE_TILE\* tile = region->tiles\[idx\];.*?\nfail:' (@'
	for (UINT32 idx = 0; idx < region->numTiles; idx++)
	{
		PROGRESSIVE_TILE_PROCESS_WORK_PARAM* param = &progressive->params[idx];
		param->progressive = progressive;
		param->region = region;
		param->context = context;
		param->tile = region->tiles[idx];
	}

	if (!progressive->rfx_context->priv->UseThreads)
	{
		/* Serial: one call per tile, exactly as before the chunking change. */
		for (UINT32 idx = 0; idx < region->numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);

		goto fail;
	}

	{
		PROGRESSIVE_TILE_CHUNK_PARAM chunks[HMRDP_TILE_CHUNKS];
		volatile UINT32 nextTile = 0;
		const UINT32 numTiles = region->numTiles;
		const UINT32 numChunks = numTiles < HMRDP_TILE_CHUNKS ? numTiles : HMRDP_TILE_CHUNKS;

		for (UINT32 c = 0; c < numChunks; c++)
		{
			PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
			chunk->params = progressive->params;
			chunk->next = &nextTile;
			chunk->numTiles = numTiles;

			progressive->work_objects[c] =
			    CreateThreadpoolWork(progressive_process_tile_chunk_callback, (void*)chunk,
			                         &progressive->rfx_context->priv->ThreadPoolEnv);
			if (!progressive->work_objects[c])
			{
				WLog_Print(progressive->log, WLOG_ERROR,
				           "Failed to create ThreadpoolWork chunk %" PRIu32, c);
				status = -1;
				break;
			}

			SubmitThreadpoolWork(progressive->work_objects[c]);
			close_cnt = c + 1;
		}

		HmrdpProgStat[1] += hmrdp_now_ns() - ht1; /* dispatch (serial) */
		const unsigned long long ht2 = hmrdp_now_ns();

		for (UINT32 c = 0; c < close_cnt; c++)
		{
			/* Only the long waits are summed: they are sequential, so their sum
			 * is the real wall time of the parallel section, while the short ones
			 * are pure per-item overhead (the futex round trip + the free). */
			const unsigned long long w0 = hmrdp_now_ns();
			WaitForThreadpoolWorkCallbacks(progressive->work_objects[c], FALSE);
			CloseThreadpoolWork(progressive->work_objects[c]);
			const unsigned long long wdt = hmrdp_now_ns() - w0;
			if (wdt > 20000ull)
				HmrdpProgStat[7] += wdt;
		}

		HmrdpProgStat[2] += hmrdp_now_ns() - ht2; /* wait+close (serial) */
	}

fail:
'@) 'volatile UINT32 nextTile = 0;'

# (a4) update_tiles: no per-tile REGION16, one visit per tile per pass.
Patch-Regex $progC '\tBOOL rc = TRUE;\n\tREGION16 clippingRects = \{ 0 \};\n\tregion16_init\(&clippingRects\);\n' (@'
	BOOL rc = TRUE;
	const unsigned long long ut0 = hmrdp_now_ns();
	HmrdpProgStat[5]++;
	REGION16 clippingRects = { 0 };
	region16_init(&clippingRects);
'@) 'HmrdpProgStat[5]++'

Patch-Regex $progC '\tfor \(UINT32 i = 0; i < surface->numUpdatedTiles; i\+\+\)\n\t\{\n\t\tUINT32 nbUpdateRects = 0;.*?\n\t\tregion16_uninit\(&updateRegion\);\n\t\ttile->dirty = FALSE;\n\t\}\n' (@'
	/*
	 * HmRdp: the per-tile clipping used to go through region16
	 * (region16_init/intersect_rect/rects/uninit), i.e. two allocations and a
	 * free per visited tile, on the order of a thousand times per full-screen
	 * frame. The result is the same set of pixels - clippingRects is a canonical
	 * region, so its rect list is already non-overlapping - and reading that list
	 * once and intersecting with plain arithmetic (with the same ordered early
	 * exit region16_intersect_rect() takes) removes the allocator from the hot
	 * path without changing which pixels are written.
	 *
	 * The stamp keeps each tile visited at most once per pass: the per-frame
	 * list gets one entry per *decode*, so a tile refined by a later message of
	 * the same frame appears several times, and every occurrence used to copy the
	 * same (already final) tile data to the same place.
	 */
	const UINT32 stamp = ++surface->updateStamp;
	UINT32 nbClipping = 0;
	const RECTANGLE_16* clippingList = region16_rects(&clippingRects, &nbClipping);

	for (UINT32 i = 0; i < surface->numUpdatedTiles; i++)
	{
		RECTANGLE_16 updateRect = { 0 };

		WINPR_ASSERT(surface->updatedTileIndices);
		const UINT32 index = surface->updatedTileIndices[i];

		WINPR_ASSERT(index < surface->tilesSize);
		RFX_PROGRESSIVE_TILE* tile = surface->tiles[index];
		WINPR_ASSERT(tile);

		if (tile->updateStamp == stamp)
			continue;
		tile->updateStamp = stamp;

		updateRect.left = nXDst + tile->x;
		updateRect.top = nYDst + tile->y;
		updateRect.right = updateRect.left + 64;
		updateRect.bottom = updateRect.top + 64;

		for (UINT32 j = 0; j < nbClipping; j++)
		{
			RECTANGLE_16 common = { 0 };

			/* region16_rects() returns the region's bands ordered by top, so
			 * everything past the tile cannot intersect it. */
			if (clippingList[j].top >= updateRect.bottom)
				break;
			if (clippingList[j].bottom <= updateRect.top)
				continue;
			if (!rectangles_intersection(&clippingList[j], &updateRect, &common))
				continue;

			if (common.left < updateRect.left)
				goto fail;
			const UINT32 nXSrc = common.left - updateRect.left;
			const UINT32 nYSrc = common.top - updateRect.top;
			const UINT32 width = common.right - common.left;
			const UINT32 height = common.bottom - common.top;

			if (common.left + width > surface->width)
				goto fail;
			if (common.top + height > surface->height)
				goto fail;
			rc = freerdp_image_copy_no_overlap(
			    pDstData, DstFormat, nDstStep, common.left, common.top, width, height, tile->data,
			    progressive->format, tile->stride, nXSrc, nYSrc, NULL, FREERDP_KEEP_DST_ALPHA);
			if (!rc)
				break;

			if (invalidRegion)
				region16_union_rect(invalidRegion, invalidRegion, &common);
		}

		tile->dirty = FALSE;
		HmrdpProgStat[6]++;
	}
'@) 'clippingList'

Patch-Regex $progC 'fail:\n\tregion16_uninit\(&clippingRects\);\n\treturn rc;\n\}' (@'
fail:
	region16_uninit(&clippingRects);
	HmrdpProgStat[3] += hmrdp_now_ns() - ut0;
	return rc;
}
'@) 'HmrdpProgStat[3] +='

# (b) keep-destination-alpha 32bpp copy: one masked word per pixel.
Patch-Regex $copyC 'static INLINE pstatus_t generic_image_copy_bgrx32_bgrx32\([^;]*?\n\{\n.*?\n\treturn PRIMITIVES_SUCCESS;\n\}\n' (@'
static INLINE pstatus_t generic_image_copy_bgrx32_bgrx32(
    BYTE* WINPR_RESTRICT pDstData, UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst, UINT32 nWidth,
    UINT32 nHeight, const BYTE* WINPR_RESTRICT pSrcData, UINT32 nSrcStep, UINT32 nXSrc,
    UINT32 nYSrc, SSIZE_T srcVMultiplier, SSIZE_T srcVOffset, SSIZE_T dstVMultiplier,
    SSIZE_T dstVOffset)
{

	const SSIZE_T srcByte = 4;
	const SSIZE_T dstByte = 4;

	const UINT32 width = nWidth - nWidth % 8;

	/*
	 * HmRdp: keep-destination-alpha 32bpp copies are the hottest loop of the
	 * gdi (FreeRDP CPU) path - every decoded progressive tile copies its 64x64
	 * pixels through here, and a full-screen frame re-composites on the order of
	 * 1400 tiles. The byte-wise body below (three separate byte moves per pixel)
	 * becomes one masked 32-bit move per pixel: the destination's 4th byte is
	 * preserved and the low three are taken from the source, exactly the bytes
	 * the byte-wise loop wrote. "The low three bytes" is not a byte-order
	 * assumption - the byte-wise loop indexes raw bytes, so the masked word keeps
	 * the same three of them on either endianness.
	 *
	 * The byte-wise loop stays as the fallback for rows that are not word
	 * aligned (all real callers are: tile rows and surface scanlines are
	 * 16-byte aligned).
	 */
	const BOOL wordAligned =
	    (((size_t)pSrcData | (size_t)pDstData | nSrcStep | nDstStep) & 3u) == 0;
	if (wordAligned)
	{
		for (SSIZE_T y = 0; y < nHeight; y++)
		{
			const BYTE* WINPR_RESTRICT srcLine =
			    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
			BYTE* WINPR_RESTRICT dstLine =
			    &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];
			UINT32* WINPR_RESTRICT dstWord = (UINT32*)&dstLine[1ULL * nXDst * dstByte];
			const UINT32* WINPR_RESTRICT srcWord =
			    (const UINT32*)&srcLine[1ULL * nXSrc * srcByte];

			for (SSIZE_T x = 0; x < nWidth; x++)
				dstWord[x] = (dstWord[x] & 0xFF000000u) | (srcWord[x] & 0x00FFFFFFu);
		}

		return PRIMITIVES_SUCCESS;
	}

	for (SSIZE_T y = 0; y < nHeight; y++)
	{
		const BYTE* WINPR_RESTRICT srcLine =
		    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
		BYTE* WINPR_RESTRICT dstLine =
		    &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];

		SSIZE_T x = 0;
		WINPR_PRAGMA_UNROLL_LOOP
		for (; x < width; x++)
		{
			dstLine[(x + nXDst) * dstByte + 0] = srcLine[(x + nXSrc) * srcByte + 0];
			dstLine[(x + nXDst) * dstByte + 1] = srcLine[(x + nXSrc) * srcByte + 1];
			dstLine[(x + nXDst) * dstByte + 2] = srcLine[(x + nXSrc) * srcByte + 2];
		}
		for (; x < nWidth; x++)
		{
			dstLine[(x + nXDst) * dstByte + 0] = srcLine[(x + nXSrc) * srcByte + 0];
			dstLine[(x + nXDst) * dstByte + 1] = srcLine[(x + nXSrc) * srcByte + 1];
			dstLine[(x + nXDst) * dstByte + 2] = srcLine[(x + nXSrc) * srcByte + 2];
		}
	}

	return PRIMITIVES_SUCCESS;
}
'@) 'keep-destination-alpha 32bpp copies are the hottest loop'

# 11) HmRdp: runtime-controllable decode worker count.
#
#     (a) The WinPR pool is the only lever on decode parallelism on this platform
#         (no thread affinity / cluster control), so the worker count becomes a
#         runtime value: `HmrdpSetDecodeThreads(n)` stores a request, the decoder
#         applies it at a Progressive message boundary (see (c)) where no work
#         item can be in flight. `n == 1` means "decode on the receiving thread
#         with no pool at all" - the power-optimal end of the range.
#     (b) The pool's built-in default stays min(cores, 4), the measured sweet
#         spot; the app overrides it with the value it derived (performance-core
#         count when the device exposes it).
#     (c) `HmrdpGetDecodeThreads() <= 1` takes the serial branch of the tile
#         decode, i.e. one worker means "no worker pool at all".
$poolC = "$Source\winpr\libwinpr\pool\pool.c"
$poolApi = @'
/* ---- HmRdp: decode worker count ----------------------------------------- */
/* The Progressive tile decode is the only user of the codec's pool. The app
 * chooses the count (0 = keep the built-in default) and the decoder applies it
 * at a region boundary, so a running decode never loses workers mid-flight. */
static volatile LONG g_HmrdpDecodeThreads = 0;
static volatile LONG g_HmrdpDecodeThreadsApplied = -1;
/* Dev counters, read back by the app's replay stats. */
static volatile LONG g_HmrdpDecodeResizes = 0;
static volatile LONG g_HmrdpDecodeApplies = 0;

static DWORD HmrdpDefaultDecodeThreads(void)
{
	SYSTEM_INFO info = { 0 };
	GetSystemInfo(&info);
	if (info.dwNumberOfProcessors < 1)
		info.dwNumberOfProcessors = 1;
	/* HmRdp: cap the per-pool fan-out. More workers do not reduce the decode
	 * wall time on this class of device (measured: 8 workers were no faster
	 * than 4) - the extra ones only land on the little cores and get woken for
	 * every message, so the default stays at the performance-core count. */
	if (info.dwNumberOfProcessors > 4)
		info.dwNumberOfProcessors = 4;
	return info.dwNumberOfProcessors;
}

WINPR_API void HmrdpSetDecodeThreads(DWORD workers)
{
	if (workers > 64)
		workers = 64;
	__atomic_store_n(&g_HmrdpDecodeThreads, (LONG)workers, __ATOMIC_RELAXED);
}

/* The worker count in effect: the app's request, or the built-in default. */
WINPR_API DWORD HmrdpGetDecodeThreads(void)
{
	const LONG requested = __atomic_load_n(&g_HmrdpDecodeThreads, __ATOMIC_RELAXED);
	if (requested <= 0)
		return HmrdpDefaultDecodeThreads();
	return (DWORD)requested;
}

/* Resizes `pool` to the requested worker count. The decoder passes its own codec
 * pool (rfx.c creates one per codec context, sized at creation from the same
 * resolver) and calls this at a region boundary, i.e. with nothing in flight. */
WINPR_API void HmrdpApplyDecodeThreads(PTP_POOL pool)
{
	const DWORD want = HmrdpGetDecodeThreads();

	__atomic_add_fetch(&g_HmrdpDecodeApplies, 1, __ATOMIC_RELAXED);

	if (!pool)
		return;

	if ((LONG)want == __atomic_load_n(&g_HmrdpDecodeThreadsApplied, __ATOMIC_RELAXED))
		return;

	/* Grow first (that only creates workers). A shrink has to go through
	 * SetThreadpoolThreadMaximum: it tears the extra workers down (they join) and
	 * recreates ThreadMinimum of them, so Minimum must already be the new count. */
	if (!SetThreadpoolThreadMinimum(pool, want))
		return;
	SetThreadpoolThreadMaximum(pool, want);
	__atomic_store_n(&g_HmrdpDecodeThreadsApplied, (LONG)want, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_HmrdpDecodeResizes, 1, __ATOMIC_RELAXED);
}

/* Dev read-out: out = { requested, applies, resizes }. */
WINPR_API void HmrdpGetDecodeThreadsStats(DWORD out[3])
{
	if (!out)
		return;

	out[0] = HmrdpGetDecodeThreads();
	out[1] = (DWORD)__atomic_load_n(&g_HmrdpDecodeApplies, __ATOMIC_RELAXED);
	out[2] = (DWORD)__atomic_load_n(&g_HmrdpDecodeResizes, __ATOMIC_RELAXED);
}

static DWORD WINAPI thread_pool_work_func(LPVOID arg)
'@
# (b) the pool's initial size comes from the same resolver (the "cap the fan-out"
#     note above is what this replaces; its marker text stays for the step above).
#
#     NOTE: this runs *before* the API insert below on purpose - both blocks
#     contain the same `SYSTEM_INFO info = { 0 };` prologue, and the pattern here
#     is non-greedy, so applying the API first would make this match (and then
#     wipe out) the API block instead of the pool init.
Patch-Regex $poolC '\tSYSTEM_INFO info = \{ 0 \};\n\tGetSystemInfo\(&info\);.*?\tSetThreadpoolThreadMaximum\(pool, info\.dwNumberOfProcessors\);\n' (@'
	const DWORD threads = HmrdpGetDecodeThreads();
	/* HmRdp: cap the per-pool fan-out. The worker count is a runtime value now
	 * (HmrdpSetDecodeThreads); this is only the starting point when the app has
	 * not chosen one. Its default, min(cores, 4), is the measured sweet spot -
	 * see doc_agent/gfx-engine.md §3. */
	if (!SetThreadpoolThreadMinimum(pool, threads))
		goto fail;
	SetThreadpoolThreadMaximum(pool, threads);
	__atomic_store_n(&g_HmrdpDecodeThreadsApplied, (LONG)threads, __ATOMIC_RELAXED);
'@) 'const DWORD threads = HmrdpGetDecodeThreads();'

# (a) the runtime API itself ($poolApi already ends with the anchor line).
Patch-Regex $poolC 'static DWORD WINAPI thread_pool_work_func\(LPVOID arg\)' $poolApi `
  'HmrdpSetDecodeThreads'

# (c) the decoder applies the request at a message boundary.
Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/* Exported by the patched libwinpr: the worker count the app asked for, and the
 * point where it is safe to resize the pool (no work item in flight yet). */
extern DWORD HmrdpGetDecodeThreads(void);
extern void HmrdpApplyDecodeThreads(void);

static INLINE SSIZE_T progressive_process_tiles(
'@) 'HmrdpApplyDecodeThreads'

Patch-Regex $progC '\tif \(!progressive->rfx_context->priv->UseThreads\)\n\t\{\n\t\t/\* Serial: one call per tile, exactly as before the chunking change\. \*/\n' (@'
	/* HmRdp: apply the app's worker-count choice here - this is the only place
	 * that submits to the pool, so no work item can be in flight. */
	HmrdpApplyDecodeThreads();

	if (!progressive->rfx_context->priv->UseThreads || HmrdpGetDecodeThreads() <= 1)
	{
		/* Serial (or forced to one worker): one call per tile, no pool at all. */
'@ + "`n") 'HmrdpGetDecodeThreads() <= 1'

# 12) HmRdp: do not re-allocate the PLANAR codec's scratch on every ResetGraphics.
#
#     `freerdp_bitmap_planar_context_reset` unconditionally (re)allocates and
#     zeroes four buffers sized from the desktop: planesBuffer x4, pTempData x6,
#     deltaPlanesBuffer x4, rlePlanesBuffer x4. On a 3120x2080 desktop that is
#     ~117MB of calloc/zeroing per call, and one ResetGraphics reaches it once per
#     codec set (the rdp context's and the GFX context's), i.e. ~130ms per event -
#     measured on device (141.8ms / 128.6ms) for a codec this client never uses
#     (Progressive / ClearCodec / uncompressed only). See
#     doc_agent/cpu-path.md §6.5.
#
#     The buffers are per-message working memory (written before read), so when
#     the geometry did not change there is nothing to do. `planes[0] != NULL`
#     witnesses that the previous call allocated them, so a failed allocation
#     still falls through to the real path.
$planarC = "$Source\libfreerdp\codec\planar.c"
Patch-Regex $planarC '\tcontext->bgr = FALSE;\n\tcontext->maxWidth = PLANAR_ALIGN\(width, 4\);\n' (@'
	context->bgr = FALSE;

	/* HmRdp: the four scratch buffers below are per-message working memory - their
	 * content is written before it is read - so when the geometry did not change
	 * they neither have to be re-allocated nor re-zeroed. Upstream does both
	 * unconditionally, which is ~117MB of calloc + zeroing per call on a
	 * 3120x2080 desktop; a single ResetGraphics reaches this once per codec set
	 * (two sets exist) and cost ~130ms per event, for a codec this client never
	 * uses (see doc_agent/cpu-path.md §6.5). `planes[0] != NULL`
	 * means the previous call allocated them, so a failure still falls through. */
	{
		const UINT32 newWidth = PLANAR_ALIGN(width, 4);
		const UINT32 newHeight = PLANAR_ALIGN(height, 4);
		if ((context->planes[0] != NULL) && (newWidth == context->maxWidth) &&
		    (newHeight == context->maxHeight))
			return TRUE;
	}

	context->maxWidth = PLANAR_ALIGN(width, 4);
'@) 'scratch buffers below are per-message working memory'

# 13) HmRdp: let the desktop-mirror surface compose straight into the primary buffer.
#
#     A full-screen GFX session has exactly one surface, mapped to the output at
#     (0,0) with no scaling, whose format and row pitch equal gdi's primary buffer.
#     That surface *is* the desktop, so gdi_OutputUpdate's per-rect
#     freerdp_image_scale() into the primary just moves ~20MB/frame from one buffer
#     to another - measured 1.3ms/frame and 41MB of DRAM traffic per frame on a
#     3120x2080 stream (doc_agent/cpu-path.md §6.1 ②). With this step the surface is
#     given the primary buffer instead, so the decoder writes the very pixels the
#     presenter uploads and the copy disappears (cpu-path.md §7 item 2).
#
#     The sharing test is `surface->data == gdi->primary_buffer`. It stays valid
#     because the only path that replaces the primary buffer (gdi_ResetGraphics ->
#     update->DesktopResize -> gdi_resize) re-points or unshares every shared
#     surface *before* the old buffer goes away, and DeleteSurface never frees a
#     shared one. Anything that does not hold exactly - a second surface, an
#     offset or scaled mapping, a different format/pitch - falls back to the plain
#     copy, so the worst case is the behaviour we had before.
$gdiGfxC = "$Source\libfreerdp\gdi\gfx.c"

$shareHelpers = @'
/* HmRdp: is this surface composing into the primary buffer itself? */
static BOOL gdi_surface_shares_primary(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	return (gdi != NULL) && (surface != NULL) && (gdi->primary_buffer != NULL) &&
	       (surface->data == gdi->primary_buffer);
}

/* HmRdp: byte-identical layout to the desktop, without requiring the mapping. */
static BOOL gdi_surface_matches_geometry(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	if ((gdi == NULL) || (surface == NULL) || (gdi->primary_buffer == NULL))
		return FALSE;
	return (surface->format == gdi->dstFormat) && (surface->width == (UINT32)gdi->width) &&
	       (surface->height == (UINT32)gdi->height) && (surface->scanline == gdi->stride);
}

/* HmRdp: and mapped 1:1 over the whole output at the origin, i.e. the desktop. */
static BOOL gdi_surface_is_desktop_mirror(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	if (!gdi_surface_matches_geometry(gdi, surface))
		return FALSE;
	if (!surface->outputMapped || (surface->outputOriginX != 0) || (surface->outputOriginY != 0))
		return FALSE;
	if ((surface->mappedWidth != (UINT32)gdi->width) ||
	    (surface->mappedHeight != (UINT32)gdi->height))
		return FALSE;
	return (surface->outputTargetWidth == surface->mappedWidth) &&
	       (surface->outputTargetHeight == surface->mappedHeight);
}

/* HmRdp: give the surface its own buffer back, carrying the pixels it showed. Must
 * run while the shared primary buffer is still alive. A sharing surface is never
 * freed (gdi_DeleteSurface), so only this allocates. */
static void gdi_surface_unshare(rdpGdi* gdi, gdiGfxSurface* surface)
{
	size_t bytes = 0;
	BYTE* own = NULL;
	UINT32 rows = 0;
	UINT32 cols = 0;
	UINT32 y = 0;

	if (!gdi_surface_shares_primary(gdi, surface))
		return;

	bytes = (size_t)surface->scanline * surface->height;
	if (bytes == 0)
		return;

	own = (BYTE*)winpr_aligned_malloc(bytes, 16);
	if (own == NULL)
	{
		/* Out of memory: keep the (still valid) primary rather than leave the
		 * surface without a buffer at all. */
		WLog_ERR(TAG, "HmRdp: cannot unshare the desktop-mirror surface");
		return;
	}
	memset(own, 0xFF, bytes);
	rows = MIN((UINT32)surface->height, (UINT32)gdi->height);
	cols = MIN(surface->scanline, gdi->stride);
	for (y = 0; y < rows; y++)
		memcpy(&own[(size_t)y * surface->scanline], &gdi->primary_buffer[(size_t)y * gdi->stride],
		       cols);
	surface->data = own;
}

/* HmRdp: every surface that currently composes into the primary buffer. */
static void gdi_gfx_unshare_all(RdpgfxClientContext* context, rdpGdi* gdi)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	UINT32 i = 0;

	if ((context == NULL) || (context->GetSurfaceIds == NULL) ||
	    (context->GetSurfaceData == NULL))
		return;
	context->GetSurfaceIds(context, &ids, &count);
	for (i = 0; i < count; i++)
	{
		gdiGfxSurface* surface =
		    (gdiGfxSurface*)context->GetSurfaceData(context, ids[i]);
		if (gdi_surface_shares_primary(gdi, surface))
			gdi_surface_unshare(gdi, surface);
	}
	free(ids);
}

static UINT32 gdi_gfx_surface_count(RdpgfxClientContext* context)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	if ((context == NULL) || (context->GetSurfaceIds == NULL))
		return 0;
	context->GetSurfaceIds(context, &ids, &count);
	free(ids);
	return count;
}

/* HmRdp: the ids of the surfaces that compose into the primary buffer, collected
 * before the primary is rebuilt (afterwards the pointer no longer identifies
 * them). Returns how many were written. */
static UINT32 gdi_gfx_collect_shared(RdpgfxClientContext* context, rdpGdi* gdi, UINT16* out,
                                     UINT32 capacity)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	UINT32 found = 0;
	UINT32 i = 0;

	if ((out == NULL) || (capacity == 0) || (context == NULL) ||
	    (context->GetSurfaceIds == NULL) || (context->GetSurfaceData == NULL))
		return 0;
	context->GetSurfaceIds(context, &ids, &count);
	for (i = 0; i < count && found < capacity; i++)
	{
		gdiGfxSurface* surface =
		    (gdiGfxSurface*)context->GetSurfaceData(context, ids[i]);
		if (gdi_surface_shares_primary(gdi, surface))
			out[found++] = ids[i];
	}
	free(ids);
	return found;
}

static BOOL gdi_gfx_id_in(const UINT16* ids, UINT32 count, UINT16 id)
{
	UINT32 i = 0;
	for (i = 0; i < count; i++)
	{
		if (ids[i] == id)
			return TRUE;
	}
	return FALSE;
}

'@

$resetDoc = @'
/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gdi_ResetGraphics(RdpgfxClientContext* context,
'@
Patch-Block $gdiGfxC $resetDoc ($shareHelpers + $resetDoc) `
  'HmRdp: is this surface composing into the primary buffer itself?'

# The primary buffer is rebuilt below (DesktopResize -> gdi_resize), so note which
# surfaces are composing into it while the pointer still identifies them.
$resetOld = @'
	if (update)
	{
		WINPR_ASSERT(update->DesktopResize);
		update->DesktopResize(gdi->context);
	}
'@
$resetNew = @'
	/* HmRdp: the primary buffer is about to be rebuilt; remember who is sharing it
	 * (the pointer stops identifying them once it is freed). */
	UINT16 sharedIds[64] = { 0 };
	UINT32 sharedCount = gdi_gfx_collect_shared(context, gdi, sharedIds,
	                                            (UINT32)(sizeof(sharedIds) / sizeof(sharedIds[0])));

	if (update)
	{
		WINPR_ASSERT(update->DesktopResize);
		update->DesktopResize(gdi->context);
	}
'@
Patch-Block $gdiGfxC $resetOld $resetNew 'HmRdp: the primary buffer is about to be rebuilt'

# Re-point the surfaces that still match the (new) desktop; the rest get their own
# buffer back. Either way the 0xFF reset below only ever touches their own memory.
$resetLoopOld = @'
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
	}

	free(pSurfaceIds);
'@
$resetLoopNew = @'
		if (gdi_gfx_id_in(sharedIds, sharedCount, surface->surfaceId))
		{
			/* Shared: either the new primary fits it exactly (compose into it from
			 * now on - it is already 0xFF, so no reset) or it takes its own buffer
			 * back and is reset like any other surface. */
			if (gdi_surface_matches_geometry(gdi, surface))
			{
				surface->data = gdi->primary_buffer;
				region16_clear(&surface->invalidRegion);
				continue;
			}
			gdi_surface_unshare(gdi, surface);
		}
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
	}

	free(pSurfaceIds);
'@
Patch-Block $gdiGfxC $resetLoopOld $resetLoopNew 'Shared: either the new primary fits it exactly'

# A mapping that is not the full desktop 1:1 takes the buffer back before the copy.
$outUpdateOld = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;
'@
$outUpdateNew = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;

	/* HmRdp: a shared surface must be the desktop, or it composes like any other
	 * (the sharing is decided when the surface is created, before the mapping that
	 * makes it a mirror is known). */
	if (gdi_surface_shares_primary(gdi, surface) && !gdi_surface_is_desktop_mirror(gdi, surface))
		gdi_surface_unshare(gdi, surface);
'@
Patch-Block $gdiGfxC $outUpdateOld $outUpdateNew 'or it composes like any other'

# ... and the copy itself is skipped when the pixels are already in place.
$scaleOld = @'
		if (!freerdp_image_scale(gdi->primary_buffer, gdi->dstFormat, gdi->stride, nXDst, nYDst,
		                         dwidth, dheight, surface->data, surface->format, surface->scanline,
		                         nXSrc, nYSrc, swidth, sheight))
		{
			rc = CHANNEL_RC_NULL_DATA;
			goto fail;
		}
'@
$scaleNew = @'
		/* HmRdp: the source *is* the destination for a desktop mirror, so copying it
		 * would only move ~20MB/frame through memory (the invalid region below is
		 * still what tells the presenter what changed). */
		if (!gdi_surface_shares_primary(gdi, surface))
		{
			if (!freerdp_image_scale(gdi->primary_buffer, gdi->dstFormat, gdi->stride, nXDst, nYDst,
			                         dwidth, dheight, surface->data, surface->format, surface->scanline,
			                         nXSrc, nYSrc, swidth, sheight))
			{
				rc = CHANNEL_RC_NULL_DATA;
				goto fail;
			}
		}
'@
Patch-Block $gdiGfxC $scaleOld $scaleNew 'would only move ~20MB/frame through memory'

# A lone, desktop-sized surface becomes the primary buffer itself.
$createOld = @'
	memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
	region16_init(&surface->invalidRegion);
'@
$createNew = @'
	/* HmRdp: one surface that is exactly the desktop *is* the desktop - compose
	 * into the primary buffer and skip the per-frame copy. Another surface means
	 * the output is no longer a single mirror, so any sharing is dropped first (the
	 * composite would otherwise overwrite the shared surface's own pixels). */
	gdi_gfx_unshare_all(context, gdi);
	if ((gdi_gfx_surface_count(context) == 0) && gdi_surface_matches_geometry(gdi, surface))
	{
		winpr_aligned_free(surface->data);
		surface->data = gdi->primary_buffer;
	}
	else
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
	region16_init(&surface->invalidRegion);
'@
Patch-Block $gdiGfxC $createOld $createNew 'one surface that is exactly the desktop *is* the desktop'

# DeleteSurface must not free the primary buffer.
$deleteDeclOld = @'
	UINT rc = CHANNEL_RC_OK;
	UINT res = ERROR_INTERNAL_ERROR;
	rdpCodecs* codecs = NULL;
	gdiGfxSurface* surface = NULL;
'@
$deleteDeclNew = @'
	UINT rc = CHANNEL_RC_OK;
	UINT res = ERROR_INTERNAL_ERROR;
	rdpCodecs* codecs = NULL;
	gdiGfxSurface* surface = NULL;
	rdpGdi* gdi = (rdpGdi*)context->custom;
'@
Patch-Block $gdiGfxC $deleteDeclOld $deleteDeclNew "`tUINT res = ERROR_INTERNAL_ERROR;`n`trdpCodecs* codecs = NULL;`n`tgdiGfxSurface* surface = NULL;"

$deleteOld = @'
		region16_uninit(&surface->invalidRegion);
		codecs = surface->codecs;
		winpr_aligned_free(surface->data);
		free(surface);
'@
$deleteNew = @'
		region16_uninit(&surface->invalidRegion);
		codecs = surface->codecs;
		/* HmRdp: a shared surface's buffer belongs to gdi's primary, not to it. */
		if (!gdi_surface_shares_primary(gdi, surface))
			winpr_aligned_free(surface->data);
		surface->data = NULL;
		free(surface);
'@
Patch-Block $gdiGfxC $deleteOld $deleteNew 'a shared surface''s buffer belongs to gdi''s primary'

Write-Host "FreeRDP OHOS patches applied to $Source"

