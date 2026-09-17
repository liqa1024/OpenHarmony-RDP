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
#    另外导出 HmrdpProgStat[16]（每条消息只碰几次，不在 per-tile 路径上）供 app 的回放
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
 *   [0] tile read/parse               [4] (unused)
 *   [1] pool dispatch                 [5] update_tiles calls
 *   [2] pool section                  [6] tiles composited
 *   [3] update_tiles                  [7] of [2], the part that really blocked
 *   [8] tiles decoded                 [9] serial loop time / worker busy ns
 *   [10..15] per-phase split (sampled)[16] tiles sampled
 *
 * `read` / `update` / the counters are filled on both paths; `dispatch` / the
 * pool section only exist when the decode runs on pool workers (they are 0 on
 * the serial branch, which is what [8]/[9] describe instead).
 */
unsigned long long HmrdpProgStat[24] = { 0 };

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

# (a3a) count the tiles the chunked dispatch decoded, and how long the workers
#       spent on them. Read per chunk, never per tile. NOTE the figure is a
#       *summed* (multi-threaded) CPU time, not a per-frame wall clock: the app
#       reports it only when the decode ran serially (see its `dec` field).
Patch-Regex $progC '\tPROGRESSIVE_TILE_CHUNK_PARAM\* chunk = \(PROGRESSIVE_TILE_CHUNK_PARAM\*\)context;\n\n\tWINPR_ASSERT\(chunk\);\n\n\tfor \(;;\)\n\t\{\n\t\tconst UINT32 index = __sync_fetch_and_add\(chunk->next, 1u\);\n\t\tif \(index >= chunk->numTiles\)\n\t\t\tbreak;\n\n\t\tprogressive_process_tiles_tile_work_callback\(instance, &chunk->params\[index\], work\);\n\t\}\n\}\n' (@'
	PROGRESSIVE_TILE_CHUNK_PARAM* chunk = (PROGRESSIVE_TILE_CHUNK_PARAM*)context;
	const unsigned long long c0 = hmrdp_now_ns();
	UINT32 done = 0;

	WINPR_ASSERT(chunk);

	for (;;)
	{
		const UINT32 index = __sync_fetch_and_add(chunk->next, 1u);
		if (index >= chunk->numTiles)
			break;

		progressive_process_tiles_tile_work_callback(instance, &chunk->params[index], work);
		done++;
	}

	/* Per chunk (not per tile): two clock reads here cost nothing measurable,
	 * while a per-tile pair would show up in the figures it reports. */
	__atomic_add_fetch(&HmrdpProgStat[8], done, __ATOMIC_RELAXED);
	__atomic_add_fetch(&HmrdpProgStat[9], hmrdp_now_ns() - c0, __ATOMIC_RELAXED);
}
'@) '__atomic_add_fetch(&HmrdpProgStat[8], done'

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
	 * see doc_agent/cpu-path.md §5. */
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

# (d) time the serial tile loop too. Without it the serial branch only shows the
#     parse (`read`) and `update_tiles` costs: the parallel path's dispatch/wait
#     counters are 0 there, so the per-tile decode - by far the dominant cost of
#     a whole-screen frame - would be invisible, and the `prog` line would look
#     like the decode is nearly free whenever the worker count is 1.
Patch-Regex $progC '\tif \(!progressive->rfx_context->priv->UseThreads \|\| HmrdpGetDecodeThreads\(\) <= 1\)\n\t\{\n\t\t/\* Serial \(or forced to one worker\): one call per tile, no pool at all\. \*/\n\t\tfor \(UINT32 idx = 0; idx < region->numTiles; idx\+\+\)\n\t\t\tprogressive_process_tiles_tile_work_callback\(0, &progressive->params\[idx\], 0\);\n\n\t\tgoto fail;\n\t\}\n' (@'
	if (!progressive->rfx_context->priv->UseThreads || HmrdpGetDecodeThreads() <= 1)
	{
		/* Serial (or forced to one worker): one call per tile, no pool at all.
		 * HmRdp dev: the tiles and the time spent decoding them are counted
		 * here - the pool-path counters above stay 0 on this branch. */
		const unsigned long long s0 = hmrdp_now_ns();
		for (UINT32 idx = 0; idx < region->numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);

		__atomic_add_fetch(&HmrdpProgStat[8], region->numTiles, __ATOMIC_RELAXED);
		__atomic_add_fetch(&HmrdpProgStat[9], hmrdp_now_ns() - s0, __ATOMIC_RELAXED);
		goto fail;
	}
'@) '__atomic_add_fetch(&HmrdpProgStat[9]'

# (e0) widen the counter array: the per-phase split below needs [10..16], and an
#      older tree only has 16 slots.
Patch-Regex $progC 'unsigned long long HmrdpProgStat\[16\] = \{ 0 \};\n' (@'
unsigned long long HmrdpProgStat[24] = { 0 };
'@) 'HmrdpProgStat[24]'

# (e) dev-only: where the time inside one decoded tile goes. A clock read is not
#     free on this platform, so a phase is only timed for one tile in 16
#     (`HMRDP_PHASE_ARM`, the sampled count lands in [4]); the app scales the
#     totals by that count. Both decode paths are covered:
#       [10] rlgr bit decode           [13] coefficient state copies
#       [11] dequant + differential    [14] upgrade (type 2/3) refinement
#       [12] inverse DWT               [15] colour transform + tile write
Patch-Regex $progC '\treturn \(\(unsigned long long\)ts\.tv_sec \* 1000000000ull\) \+ \(unsigned long long\)ts\.tv_nsec;\n\}\n' (@'
	return ((unsigned long long)ts.tv_sec * 1000000000ull) + (unsigned long long)ts.tv_nsec;
}

/*
 * HmRdp dev: per-phase sampling of the tile decode (see the patch note in
 * native/scripts/patch-freerdp.ps1 step 11e). `HMRDP_PHASE_ARM()` is called once
 * per decoded tile; a phase is only instrumented on the sampled tiles, so the
 * clock reads stay out of the figures they report. Designed for the serial
 * branch: with pool workers the `g_HmrdpSampleTile` flag would race (harmless for
 * a dev probe, but then the split is not per-tile exact).
 */
static UINT32 g_HmrdpTileSample = 0;
static BOOL g_HmrdpSampleTile = FALSE;
#define HMRDP_PHASE_ARM() \
	g_HmrdpSampleTile = ((__atomic_add_fetch(&g_HmrdpTileSample, 1, __ATOMIC_RELAXED) & 15u) == 0)

static INLINE unsigned long long hmrdp_phase_begin(void)
{
	return g_HmrdpSampleTile ? hmrdp_now_ns() : 0;
}

static INLINE void hmrdp_phase_end(int slot, unsigned long long t0)
{
	if (t0 != 0)
		__atomic_add_fetch(&HmrdpProgStat[slot], hmrdp_now_ns() - t0, __ATOMIC_RELAXED);
}
'@) 'HMRDP_PHASE_ARM'

Patch-Regex $progC '\tstatus = progressive->rfx_context->rlgr_decode\(RLGR1, data, length, buffer, 4096\);\n\n\tif \(status < 0\)\n\t\treturn status;\n\n\tCopyMemory\(sign, buffer, 4096ULL \* 2ULL\);\n\tif \(!extrapolate\)\n\t\{\n\t\trfx_differential_decode\(buffer \+ 4032, 64\);\n' (@'
	const unsigned long long p_rlgr = hmrdp_phase_begin();
	status = progressive->rfx_context->rlgr_decode(RLGR1, data, length, buffer, 4096);
	hmrdp_phase_end(10, p_rlgr);

	if (status < 0)
		return status;

	const unsigned long long p_sign = hmrdp_phase_begin();
	CopyMemory(sign, buffer, 4096ULL * 2ULL);
	hmrdp_phase_end(13, p_sign);

	const unsigned long long p_deq = hmrdp_phase_begin();
	if (!extrapolate)
	{
		rfx_differential_decode(buffer + 4032, 64);
'@ + "`n") 'hmrdp_phase_end(10, p_rlgr)'

Patch-Regex $progC '\t\tprogressive_rfx_decode_block\(prims, &buffer\[4015\], 81, shift->LL3\);   /\* LL3 \*/\n\t\}\n\treturn progressive_rfx_dwt_2d_decode' (@'
		progressive_rfx_decode_block(prims, &buffer[4015], 81, shift->LL3);   /* LL3 */
	}
	hmrdp_phase_end(11, p_deq);
	return progressive_rfx_dwt_2d_decode
'@ + "`n") 'hmrdp_phase_end(11, p_deq)'

Patch-Regex $progC '\tif \(reverse\)\n\t\tmemcpy\(buffer, current, bsize\);\n\telse if \(!coeffDiff\)\n\t\tmemcpy\(current, buffer, bsize\);\n\telse\n\t\tprims->add_16s_inplace\(buffer, current, belements\);\n' (@'
	const unsigned long long p_state = hmrdp_phase_begin();
	if (reverse)
		memcpy(buffer, current, bsize);
	else if (!coeffDiff)
		memcpy(current, buffer, bsize);
	else
		prims->add_16s_inplace(buffer, current, belements);
	hmrdp_phase_end(13, p_state);
'@ + "`n") 'hmrdp_phase_end(13, p_state)'

Patch-Regex $progC '\tif \(!extrapolate\)\n\t\{\n\t\tprogressive->rfx_context->dwt_2d_decode\(buffer, temp\);\n\t\}\n\telse\n\t\{\n\t\tWINPR_ASSERT\(progressive->rfx_context->dwt_2d_extrapolate_decode\);\n\t\tprogressive->rfx_context->dwt_2d_extrapolate_decode\(buffer, temp\);\n\t\}\n' (@'
	const unsigned long long p_idwt = hmrdp_phase_begin();
	if (!extrapolate)
	{
		progressive->rfx_context->dwt_2d_decode(buffer, temp);
	}
	else
	{
		WINPR_ASSERT(progressive->rfx_context->dwt_2d_extrapolate_decode);
		progressive->rfx_context->dwt_2d_extrapolate_decode(buffer, temp);
	}
	hmrdp_phase_end(12, p_idwt);
'@ + "`n") 'hmrdp_phase_end(12, p_idwt)'

Patch-Regex $progC '\tconst INT16\*\* ptr = WINPR_REINTERPRET_CAST\(pSrcDst, INT16\*\*, const INT16\*\*\);\n\trc = prims->yCbCrToRGB_16s8u_P3AC4R\(ptr, 64 \* 2, tile->data, tile->stride, progressive->format,\n' (@'
	const INT16** ptr = WINPR_REINTERPRET_CAST(pSrcDst, INT16**, const INT16**);
	const unsigned long long p_color = hmrdp_phase_begin();
	rc = prims->yCbCrToRGB_16s8u_P3AC4R(ptr, 64 * 2, tile->data, tile->stride, progressive->format,
'@ + "`n") 'hmrdp_phase_end(15, p_color)'

Patch-Regex $progC '\t\t                                    &roi_64x64\);\nfail:\n\tBufferPool_Return\(progressive->bufferPool, pBuffer\);\n\treturn rc;\n\}\n' (@'
	                                    &roi_64x64);
	hmrdp_phase_end(15, p_color);
fail:
	BufferPool_Return(progressive->bufferPool, pBuffer);
	return rc;
}
'@ + "`n") 'hmrdp_phase_end(15, p_color)'

Patch-Regex $progC '\tstatus = progressive_rfx_upgrade_component\(progressive, &shiftY, quantProgY, &yNumBits,\n' (@'
	const unsigned long long p_up = hmrdp_phase_begin();
	status = progressive_rfx_upgrade_component(progressive, &shiftY, quantProgY, &yNumBits,
'@ + "`n") 'hmrdp_phase_end(14, p_up)'

Patch-Regex $progC '\tif \(status < 0\)\n\t\tgoto fail;\n\n\tconst INT16\*\* ptr = WINPR_REINTERPRET_CAST\(pSrcDst, INT16\*\*, const INT16\*\*\);\n\tstatus = prims->yCbCrToRGB_16s8u_P3AC4R\(ptr, 64 \* 2, tile->data, tile->stride,\n\t                                        progressive->format, &roi_64x64\);\n' (@'
	if (status < 0)
		goto fail;

	hmrdp_phase_end(14, p_up);
	const INT16** ptr = WINPR_REINTERPRET_CAST(pSrcDst, INT16**, const INT16**);
	const unsigned long long p_color = hmrdp_phase_begin();
	status = prims->yCbCrToRGB_16s8u_P3AC4R(ptr, 64 * 2, tile->data, tile->stride,
	                                        progressive->format, &roi_64x64);
	hmrdp_phase_end(15, p_color);
'@ + "`n") 'hmrdp_phase_end(14, p_up)'

Patch-Regex $progC '\tPROGRESSIVE_TILE_PROCESS_WORK_PARAM\* param = \(PROGRESSIVE_TILE_PROCESS_WORK_PARAM\*\)context;\n\n\tWINPR_UNUSED\(instance\);\n\tWINPR_UNUSED\(work\);\n' (@'
	PROGRESSIVE_TILE_PROCESS_WORK_PARAM* param = (PROGRESSIVE_TILE_PROCESS_WORK_PARAM*)context;

	WINPR_UNUSED(instance);
	WINPR_UNUSED(work);

	/* HmRdp dev: arm the per-phase sampling for this tile (1 in 16). */
	HMRDP_PHASE_ARM();
	if (g_HmrdpSampleTile)
		__atomic_add_fetch(&HmrdpProgStat[16], 1, __ATOMIC_RELAXED);
'@ + "`n") 'HMRDP_PHASE_ARM();'

# 12) HmRdp: do not re-allocate the PLANAR codec's scratch on every ResetGraphics.
#
#     `freerdp_bitmap_planar_context_reset` unconditionally (re)allocates and
#     zeroes four buffers sized from the desktop: planesBuffer x4, pTempData x6,
#     deltaPlanesBuffer x4, rlePlanesBuffer x4. On a 3120x2080 desktop that is
#     ~117MB of calloc/zeroing per call, and one ResetGraphics reaches it once per
#     codec set (the rdp context's and the GFX context's), i.e. ~130ms per event -
#     measured on device (141.8ms / 128.6ms) for a codec this client never uses
#     (Progressive / ClearCodec / uncompressed only). See
#     doc_agent/cpu-path.md §3.
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
	 * uses (see doc_agent/cpu-path.md §3). `planes[0] != NULL`
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
#     3120x2080 stream (doc_agent/cpu-path.md §4). With this step the surface is
#     given the primary buffer instead, so the decoder writes the very pixels the
#     presenter uploads and the copy disappears (cpu-path.md §4).
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
Patch-Block $gdiGfxC $deleteDeclOld $deleteDeclNew "`tgdiGfxSurface* surface = NULL;`r`n`trdpGdi* gdi = (rdpGdi*)context->custom;"

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

# 14) HmRdp: record the dirty area as one span per tile row instead of one
#     region16 union per tile.
#
#     `update_tiles` calls region16_union_rect() once per decoded tile, into the
#     surface's persistent invalidRegion. The union is O(region) and the region
#     grows all frame (to roughly one band per tile), so a full-screen frame is
#     O(n^2) - measured 6.4ms/frame of *serial* RDP-thread time, ~30% of a frame on
#     the video sample, for ~1330 tiles. Measured detail: the per-message region is
#     essentially one rect per tile, so there is nothing to merge per message
#     (doc_agent/cpu-path.md §8) - the cost is the union itself, not the rect
#     count.
#
#     So the decoder stops touching the region16 per tile: it keeps one min/max
#     span per *tile row* (O(1) per tile, no allocation, no growth) and the
#     consumer folds those spans into the region once, in ~gridHeight cheap unions,
#     right before it reads the region. The covered area can only grow to whole
#     tiles, and the region's shape was never part of the semantics - only its area
#     is, and the extra pixels are the surface's own (already correct) content.
$progRowsH = "$Source\libfreerdp\codec\progressive.h"
$progRowsC = "$Source\libfreerdp\codec\progressive.c"
$progRowsApi = "$Source\include\freerdp\codec\progressive.h"
$gdiRowsC = "$Source\libfreerdp\gdi\gfx.c"

Patch-Block $progRowsH '	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */' (@'
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
	/* HmRdp: the frame's dirty area as one [left,right) span of tile *columns* per
	 * tile row (see HmrdpProgressiveFlushDirty). Recording a span is O(1); the
	 * region16 union this replaces is O(region) and runs once per tile. */
	UINT16* hmrdpDirtyLeft;
	UINT16* hmrdpDirtyRight;
	UINT32 hmrdpDirtyRows;
	BOOL hmrdpDirtyAny;
'@) 'hmrdpDirtyLeft'

Patch-Block $progRowsC @'
	winpr_aligned_free((void*)surface->tiles);
	winpr_aligned_free(surface->updatedTileIndices);
	winpr_aligned_free(surface);
'@ @'
	winpr_aligned_free((void*)surface->tiles);
	winpr_aligned_free(surface->updatedTileIndices);
	winpr_aligned_free(surface->hmrdpDirtyLeft);
	winpr_aligned_free(surface->hmrdpDirtyRight);
	winpr_aligned_free(surface);
'@ 'winpr_aligned_free(surface->hmrdpDirtyLeft);'

Patch-Block $progRowsC @'
	if (!progressive_allocate_tile_cache(surface, surface->gridSize))
	{
		progressive_surface_context_free(surface);
		return NULL;
	}
'@ @'
	if (!progressive_allocate_tile_cache(surface, surface->gridSize))
	{
		progressive_surface_context_free(surface);
		return NULL;
	}

	/* HmRdp: one dirty span per tile row. Sized from the tile grid (which can have
	 * one row/column more than the surface needs); a failure just leaves the rows
	 * at 0, and update_tiles falls back to the region16 path. */
	surface->hmrdpDirtyRows = surface->gridHeight;
	surface->hmrdpDirtyLeft =
	    (UINT16*)winpr_aligned_malloc(surface->hmrdpDirtyRows * sizeof(UINT16), 32);
	surface->hmrdpDirtyRight =
	    (UINT16*)winpr_aligned_malloc(surface->hmrdpDirtyRows * sizeof(UINT16), 32);
	if (surface->hmrdpDirtyLeft && surface->hmrdpDirtyRight)
	{
		for (UINT32 row = 0; row < surface->hmrdpDirtyRows; row++)
		{
			surface->hmrdpDirtyLeft[row] = UINT16_MAX;
			surface->hmrdpDirtyRight[row] = 0;
		}
	}
	else
	{
		winpr_aligned_free(surface->hmrdpDirtyLeft);
		winpr_aligned_free(surface->hmrdpDirtyRight);
		surface->hmrdpDirtyLeft = NULL;
		surface->hmrdpDirtyRight = NULL;
		surface->hmrdpDirtyRows = 0;
	}
'@ 'one dirty span per tile row'

Patch-Block $progRowsC @'
			if (invalidRegion)
				region16_union_rect(invalidRegion, invalidRegion, &common);
'@ @'
			/* HmRdp: O(1) damage bookkeeping instead of a region16 union (which is
			 * O(region), once per tile, against a region that grows all frame). The
			 * span covers whole tiles, i.e. a superset of what was copied - the
			 * extra pixels are the surface's own content, and the consumer clips the
			 * result to the surface. */
			if (invalidRegion && (surface->hmrdpDirtyRows > 0) && (nXDst == 0) && (nYDst == 0))
			{
				const UINT32 row = tile->yIdx;
				if (row < surface->hmrdpDirtyRows)
				{
					if (surface->hmrdpDirtyLeft[row] > tile->xIdx)
						surface->hmrdpDirtyLeft[row] = (UINT16)tile->xIdx;
					if (surface->hmrdpDirtyRight[row] < (UINT16)(tile->xIdx + 1))
						surface->hmrdpDirtyRight[row] = (UINT16)(tile->xIdx + 1);
					surface->hmrdpDirtyAny = TRUE;
				}
			}
			else if (invalidRegion)
				region16_union_rect(invalidRegion, invalidRegion, &common);
'@ 'O(1) damage bookkeeping instead of a region16 union'

# The flush the consumer calls: fold the spans into the region, then forget them.
$flushFn = @'

/* HmRdp: fold the frame's dirty tile-row spans into `out` (one rect per row that
 * has any tile) and reset them. Called by the composer right before it reads the
 * region, so the decoder never has to touch the region16 per tile - see
 * update_tiles. */
FREERDP_API BOOL HmrdpProgressiveFlushDirty(PROGRESSIVE_CONTEXT* progressive, UINT16 surfaceId,
                                            REGION16* out)
{
	PROGRESSIVE_SURFACE_CONTEXT* surface = NULL;

	if (!progressive || !out)
		return FALSE;

	surface = progressive_get_surface_data(progressive, surfaceId);
	if (!surface || !surface->hmrdpDirtyAny || !surface->hmrdpDirtyLeft || !surface->hmrdpDirtyRight)
		return TRUE;

	for (UINT32 row = 0; row < surface->hmrdpDirtyRows; row++)
	{
		RECTANGLE_16 rect = { 0 };
		if (surface->hmrdpDirtyLeft[row] >= surface->hmrdpDirtyRight[row])
			continue;
		rect.left = (UINT16)(surface->hmrdpDirtyLeft[row] * 64);
		rect.top = (UINT16)(row * 64);
		rect.right = (UINT16)(surface->hmrdpDirtyRight[row] * 64);
		rect.bottom = (UINT16)((row + 1) * 64);
		HmrdpProgStat[4]++;
		region16_union_rect(out, out, &rect);
		surface->hmrdpDirtyLeft[row] = UINT16_MAX;
		surface->hmrdpDirtyRight[row] = 0;
	}
	surface->hmrdpDirtyAny = FALSE;
	return TRUE;
}
'@
$progRowsAppendOld = 'INT32 progressive_decompress(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive,'
Patch-Block $progRowsC $progRowsAppendOld ($flushFn + $progRowsAppendOld) 'HmrdpProgressiveFlushDirty'

$progFlushDecl = @'
/* HmRdp: fold the progressive decoder's dirty tile-row spans into `out` (one cheap
 * union per dirty row, instead of one O(region) union per decoded tile). The
 * composer calls this right before it reads the surface's invalid region. */
FREERDP_API BOOL HmrdpProgressiveFlushDirty(PROGRESSIVE_CONTEXT* progressive, UINT16 surfaceId,
                                            REGION16* out);

FREERDP_API INT32 progressive_decompress(
'@
Patch-Block $progRowsApi 'FREERDP_API INT32 progressive_decompress(' `
  ($progFlushDecl + 'FREERDP_API INT32 progressive_decompress(') 'HmrdpProgressiveFlushDirty'

# ... and the composer folds them in before it clips/reads the region.
$gdiFlushCall = @'
	/* HmRdp: the progressive decoder records its damage as per-tile-row spans (see
	 * HmrdpProgressiveFlushDirty), so fold them into the region here - one cheap
	 * union per dirty row instead of one per tile inside the decode loop. */
	if (surface->codecs != NULL && surface->codecs->progressive != NULL)
		HmrdpProgressiveFlushDirty(surface->codecs->progressive, surface->surfaceId,
		                           &(surface->invalidRegion));

'@
Patch-Block $gdiRowsC "`tsurfaceX = surface->outputOriginX;" `
  ($gdiFlushCall + "`tsurfaceX = surface->outputOriginX;") `
  'HmrdpProgressiveFlushDirty(surface->codecs->progressive'

# 15) HmRdp: bit-exact inverse DWT (doc_agent/cpu-accel-plan.md, stage one C1).
#
#     The progressive decode spends about half of its tile cost in the inverse
#     DWT, and the upstream block is pure scalar int16 lifting. The rewrite below
#     changes only how the same arithmetic is organised, so the output is
#     byte-for-byte identical to the original (kept verbatim as the scalar
#     reference):
#
#       - the horizontal and the vertical pass are each split into their even- and
#         odd-sample halves - which is what the original already is: an odd sample
#         (2n+1) only ever reads the even samples next to it, never another odd
#         one, so nothing here is a new dependency;
#       - the vertical pass walks rows (x innermost) instead of columns, so every
#         access is contiguous;
#       - on AArch64 the inner loops work on eight samples at a time with the NEON
#         halving adds: VRHADD is (a + b + 1) >> 1 and VHADD is (a + b) >> 1, both
#         with the sum evaluated in full precision - exactly the `int` arithmetic
#         of the C code. The left shift and the final store keep the same INT16
#         truncation, so nothing saturates or rounds differently. (The upstream
#         NEON variant in codec/neon/rfx_neon.c does NOT have this property: it
#         adds in 16-bit lanes and wraps, which is why it stays disabled with
#         -DWITH_SIMD=OFF and is not what this step turns on.)
#
#     `subband_width` is 8, 16 or 32 for every caller, i.e. always a multiple of
#     the vector width; anything else, and every non-AArch64 target (the emulator
#     build), runs the scalar reference unchanged.
#
#     The block also carries the dev-only proof required by that gate: with
#     HmrdpSetDwtCheck(1) one tile in HMRDP_DWT_CHECK_EVERY is decoded a second
#     time with the scalar reference and the differing elements are counted into
#     HmrdpDwtCheckStat[2] = { tiles checked, elements that differed }. The golden
#     reference cannot prove this rewrite on its own (it was recorded by the very
#     decoder that changed), which is why the check exists; the app turns it on
#     for the 参考:对比 run and prints the counters.
$dwtC = "$Source\libfreerdp\codec\rfx_dwt.c"
$dwtNew = @'
/*
 * HmRdp: bit-exact restructure of the inverse DWT (see the patch note in
 * native/scripts/patch-freerdp.ps1 step 15, doc_agent/cpu-accel-plan.md C1). The
 * upstream scalar version follows verbatim and stays the reference for the
 * fallback path and for the dev comparison (HmrdpSetDwtCheck).
 */
#define HMRDP_DWT_SUBBAND_MAX 64

static INLINE void hmrdp_dwt_2d_decode_block_scalar(INT16* WINPR_RESTRICT buffer,
                                                    INT16* WINPR_RESTRICT idwt,
                                                    size_t subband_width)
{
	const size_t total_width = subband_width << 1;

	/* Inverse DWT in horizontal direction, results in 2 sub-bands in L, H order in tmp buffer idwt.
	 */
	/* The 4 sub-bands are stored in HL(0), LH(1), HH(2), LL(3) order. */
	/* The lower part L uses LL(3) and HL(0). */
	/* The higher part H uses LH(1) and HH(2). */

	const INT16* ll = buffer + subband_width * subband_width * 3;
	const INT16* hl = buffer;
	INT16* l_dst = idwt;

	const INT16* lh = buffer + subband_width * subband_width;
	const INT16* hh = buffer + subband_width * subband_width * 2;
	INT16* h_dst = idwt + subband_width * subband_width * 2;

	for (size_t y = 0; y < subband_width; y++)
	{
		/* Even coefficients */
		l_dst[0] = ll[0] - ((hl[0] + hl[0] + 1) >> 1);
		h_dst[0] = lh[0] - ((hh[0] + hh[0] + 1) >> 1);
		for (size_t n = 1; n < subband_width; n++)
		{
			const size_t x = n << 1;
			l_dst[x] = ll[n] - ((hl[n - 1] + hl[n] + 1) >> 1);
			h_dst[x] = lh[n] - ((hh[n - 1] + hh[n] + 1) >> 1);
		}

		/* Odd coefficients */
		size_t n = 0;
		for (; n < subband_width - 1; n++)
		{
			const size_t x = n << 1;
			l_dst[x + 1] = (hl[n] << 1) + ((l_dst[x] + l_dst[x + 2]) >> 1);
			h_dst[x + 1] = (hh[n] << 1) + ((h_dst[x] + h_dst[x + 2]) >> 1);
		}

		const size_t x = n << 1;
		l_dst[x + 1] = (hl[n] << 1) + (l_dst[x]);
		h_dst[x + 1] = (hh[n] << 1) + (h_dst[x]);

		ll += subband_width;
		hl += subband_width;
		l_dst += total_width;

		lh += subband_width;
		hh += subband_width;
		h_dst += total_width;
	}

	/* Inverse DWT in vertical direction, results are stored in original buffer. */
	for (size_t x = 0; x < total_width; x++)
	{
		const INT16* l = idwt + x;
		const INT16* h = idwt + x + subband_width * total_width;
		INT16* dst = buffer + x;

		*dst = *l - ((*h * 2 + 1) >> 1);

		for (size_t n = 1; n < subband_width; n++)
		{
			l += total_width;
			h += total_width;

			/* Even coefficients */
			dst[2 * total_width] = *l - ((*(h - total_width) + *h + 1) >> 1);

			/* Odd coefficients */
			dst[total_width] = (*(h - total_width) << 1) + ((*dst + dst[2 * total_width]) >> 1);

			dst += 2 * total_width;
		}

		dst[total_width] = (*h << 1) + ((*dst * 2) >> 1);
	}
}

#if defined(__aarch64__)
#include <arm_neon.h>

#define HMRDP_DWT_LANES 8

/* One row of the horizontal pass: the even samples into a scratch row, then the
 * odd ones, which are written together with their even neighbour (vst2q stores
 * the pair interleaved, i.e. dst[2n] / dst[2n+1] exactly as the scalar code
 * writes them). */
static INLINE void hmrdp_dwt_2d_decode_h_row(const INT16* WINPR_RESTRICT l,
                                             const INT16* WINPR_RESTRICT h,
                                             INT16* WINPR_RESTRICT dst, size_t subband_width)
{
	INT16 even[HMRDP_DWT_SUBBAND_MAX];
	size_t n = 0;

	for (n = 0; n < subband_width; n += HMRDP_DWT_LANES)
	{
		const int16x8_t lv = vld1q_s16(l + n);
		const int16x8_t hv = vld1q_s16(h + n);
		int16x8_t left;

		if (n == 0)
		{
			/* h[n - 1] is h[0] at the row start (the scalar code uses hl[0] twice). */
			left = vextq_s16(hv, hv, HMRDP_DWT_LANES - 1);
			left = vsetq_lane_s16(vgetq_lane_s16(hv, 0), left, 0);
		}
		else
			left = vld1q_s16(h + n - 1);

		vst1q_s16(even + n, vsubq_s16(lv, vrhaddq_s16(left, hv)));
	}

	for (n = 0; n < subband_width; n += HMRDP_DWT_LANES)
	{
		const int16x8_t ev = vld1q_s16(even + n);
		const int16x8_t hv = vld1q_s16(h + n);
		int16x8x2_t pair;
		int16x8_t next;

		if (n + HMRDP_DWT_LANES < subband_width)
			next = vld1q_s16(even + n + 1);
		else
		{
			/* The last odd sample pairs the last even sample with itself. */
			next = vextq_s16(ev, ev, 1);
			next = vsetq_lane_s16(vgetq_lane_s16(ev, HMRDP_DWT_LANES - 1), next,
			                      HMRDP_DWT_LANES - 1);
		}

		pair.val[0] = ev;
		pair.val[1] = vaddq_s16(vshlq_n_s16(hv, 1), vhaddq_s16(ev, next));
		vst2q_s16(dst + 2 * n, pair);
	}
}

/* Vertical pass, one row at a time: the even output rows first (each reads the
 * two high-band rows around it), then the odd rows (each reads the two even rows
 * around it, both of which are already final). */
static INLINE void hmrdp_dwt_2d_decode_v(const INT16* WINPR_RESTRICT l,
                                         const INT16* WINPR_RESTRICT h,
                                         INT16* WINPR_RESTRICT dst, size_t subband_width)
{
	const size_t total_width = subband_width << 1;
	size_t n = 0;
	size_t x = 0;

	for (n = 0; n < subband_width; n++)
	{
		const INT16* WINPR_RESTRICT ln = l + n * total_width;
		const INT16* WINPR_RESTRICT hn = h + n * total_width;
		const INT16* WINPR_RESTRICT hm = (n == 0) ? hn : (hn - total_width);
		INT16* WINPR_RESTRICT d = dst + (n << 1) * total_width;

		for (x = 0; x < total_width; x += HMRDP_DWT_LANES)
			vst1q_s16(d + x, vsubq_s16(vld1q_s16(ln + x),
			                           vrhaddq_s16(vld1q_s16(hm + x), vld1q_s16(hn + x))));
	}

	for (n = 0; n < subband_width; n++)
	{
		const INT16* WINPR_RESTRICT hn = h + n * total_width;
		INT16* WINPR_RESTRICT d0 = dst + (n << 1) * total_width;
		INT16* WINPR_RESTRICT d2 = ((n + 1) < subband_width) ? (d0 + (total_width << 1)) : d0;
		INT16* WINPR_RESTRICT d = d0 + total_width;

		for (x = 0; x < total_width; x += HMRDP_DWT_LANES)
			vst1q_s16(d + x, vaddq_s16(vshlq_n_s16(vld1q_s16(hn + x), 1),
			                           vhaddq_s16(vld1q_s16(d0 + x), vld1q_s16(d2 + x))));
	}
}

static INLINE void hmrdp_dwt_2d_decode_block_neon(INT16* WINPR_RESTRICT buffer,
                                                  INT16* WINPR_RESTRICT idwt,
                                                  size_t subband_width)
{
	const size_t area = subband_width * subband_width;
	const size_t total_width = subband_width << 1;
	size_t y = 0;

	for (y = 0; y < subband_width; y++)
	{
		hmrdp_dwt_2d_decode_h_row(buffer + area * 3 + y * subband_width,
		                          buffer + y * subband_width, idwt + y * total_width,
		                          subband_width);
		hmrdp_dwt_2d_decode_h_row(buffer + area + y * subband_width,
		                          buffer + area * 2 + y * subband_width,
		                          idwt + area * 2 + y * total_width, subband_width);
	}

	hmrdp_dwt_2d_decode_v(idwt, idwt + area * 2, buffer, subband_width);
}
#endif /* __aarch64__ */

static INLINE void rfx_dwt_2d_decode_block(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT idwt,
                                           size_t subband_width)
{
#if defined(__aarch64__)
	if (((subband_width % HMRDP_DWT_LANES) == 0) && (subband_width <= HMRDP_DWT_SUBBAND_MAX))
	{
		hmrdp_dwt_2d_decode_block_neon(buffer, idwt, subband_width);
		return;
	}
#endif
	hmrdp_dwt_2d_decode_block_scalar(buffer, idwt, subband_width);
}

/* ---- HmRdp dev: scalar reference vs the optimised block ------------------- */
/* Off unless the app asks for it (HmrdpSetDwtCheck). The decode is single
 * threaded (the worker count is pinned to one), so plain globals are enough; the
 * counters are read back into the app statistics line. HmrdpDwtCheckArmed() is
 * shared with the extrapolated DWT in progressive.c, which is the variant a
 * Progressive region normally uses. */
FREERDP_API unsigned long long HmrdpDwtCheckStat[2] = { 0, 0 };

static volatile LONG g_HmrdpDwtCheck = 0;
static volatile LONG g_HmrdpDwtSample = 0;
static INT16 g_HmrdpDwtRef[4096] = { 0 };
static INT16 g_HmrdpDwtScratch[4096] = { 0 };

#define HMRDP_DWT_CHECK_EVERY 16

FREERDP_API void HmrdpSetDwtCheck(int on)
{
	__atomic_store_n(&g_HmrdpDwtCheck, on ? 1 : 0, __ATOMIC_RELAXED);
}

/* True for one decode in HMRDP_DWT_CHECK_EVERY, and only while the check is on. */
FREERDP_API int HmrdpDwtCheckArmed(void)
{
	if (__atomic_load_n(&g_HmrdpDwtCheck, __ATOMIC_RELAXED) == 0)
		return 0;
	return (__atomic_add_fetch(&g_HmrdpDwtSample, 1, __ATOMIC_RELAXED) % HMRDP_DWT_CHECK_EVERY) == 0;
}

/* Collects one compared tile. */
FREERDP_API void HmrdpDwtCheckResult(unsigned int bad)
{
	__atomic_add_fetch(&HmrdpDwtCheckStat[0], 1, __ATOMIC_RELAXED);
	if (bad != 0)
		__atomic_add_fetch(&HmrdpDwtCheckStat[1], bad, __ATOMIC_RELAXED);
}

static void hmrdp_dwt_check(const INT16* buffer)
{
	const size_t elements = 4096;
	size_t i = 0;
	UINT32 bad = 0;

	/* g_HmrdpDwtRef holds the input coefficients, copied before the optimised
	 * decode ran: the decode overwrites the whole coefficient buffer, so the
	 * scalar reference has to start from the same input, not from its result. */
	hmrdp_dwt_2d_decode_block_scalar(&g_HmrdpDwtRef[3840], g_HmrdpDwtScratch, 8);
	hmrdp_dwt_2d_decode_block_scalar(&g_HmrdpDwtRef[3072], g_HmrdpDwtScratch, 16);
	hmrdp_dwt_2d_decode_block_scalar(&g_HmrdpDwtRef[0], g_HmrdpDwtScratch, 32);

	for (i = 0; i < elements; i++)
	{
		if (g_HmrdpDwtRef[i] != buffer[i])
			bad++;
	}

	HmrdpDwtCheckResult(bad);
}

void rfx_dwt_2d_decode(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT dwt_buffer)
{
	const BOOL check = HmrdpDwtCheckArmed();

	WINPR_ASSERT(buffer);
	WINPR_ASSERT(dwt_buffer);

	/* HmRdp dev: take the input before the decode overwrites it, then re-decode
	 * that copy with the scalar reference below and compare. */
	if (check)
		memcpy(g_HmrdpDwtRef, buffer, sizeof(g_HmrdpDwtRef));

	rfx_dwt_2d_decode_block(&buffer[3840], dwt_buffer, 8);
	rfx_dwt_2d_decode_block(&buffer[3072], dwt_buffer, 16);
	rfx_dwt_2d_decode_block(&buffer[0], dwt_buffer, 32);

	if (check)
		hmrdp_dwt_check(buffer);
}
'@
Patch-Regex $dwtC 'static INLINE void rfx_dwt_2d_decode_block\(INT16\* WINPR_RESTRICT buffer,\s+INT16\* WINPR_RESTRICT idwt,\n\s*size_t subband_width\)\n\{.*?\nvoid rfx_dwt_2d_decode\(INT16\* WINPR_RESTRICT buffer, INT16\* WINPR_RESTRICT dwt_buffer\)\n\{.*?\n\}\n' $dwtNew 'HmrdpDwtCheckArmed'

# 16) HmRdp: the extrapolated (reduced) inverse DWT - the one the Progressive
#     codec actually runs (doc_agent/cpu-accel-plan.md stage one C1).
#
#     `rfx_dwt_2d_extrapolate_decode` (below) is what a region with the
#     RFX_DWT_REDUCE_EXTRAPOLATE flag decodes with, and every tile of a
#     full-screen Progressive stream carries that flag (measured: the
#     non-extrapolated rfx_dwt_2d_decode() is never entered on a video capture).
#     It walks the bands with a recurrence in the *inner* loop:
#
#       X2 = L0 - (H0 + H1) / 2;   X1 = (X0 + X2) / 2 + 2 * H0;   X0 = X2;
#
#     so every sample of one column depends on the previous one. The rewrite
#     below changes nothing about that arithmetic - the INT16 truncation on every
#     assignment and the truncating `/ 2` are the original ones, so the output is
#     byte-for-byte identical - it only reorders the work:
#
#       - the X2 sequence is a plain function of the L/H samples, so it is
#         computed first (idwt_x into a scratch row, idwt_y into one scratch row
#         per column) and the two output samples per step are then written by a
#         dependency-free loop;
#       - idwt_y becomes row-major: j (the band row) is the outer loop and the
#         inner loop runs over the tile columns, so both the reads and the writes
#         are contiguous. The upstream version walks *columns*, i.e. every single
#         sample lands in a different cache line (128-byte stride), which is why
#         it costs more than its arithmetic suggests.
#
#     The upstream scalar functions are kept verbatim as `..._scalar` and are the
#     reference for the dev comparison (the extrapolate entry point runs it on a
#     sample of tiles; see step 15 for the counters). Everything is plain C - no
#     intrinsics - so the scalar build for other targets keeps working unchanged.
$progDwtC = "$Source\libfreerdp\codec\progressive.c"
$progDwtRows = @'
/*
 * HmRdp: the reduced (extrapolated) inverse DWT, restructured so its inner loops
 * are contiguous and dependency-free; the arithmetic is the upstream one (see
 * the patch note in native/scripts/patch-freerdp.ps1 step 16). The upstream
 * scalar version follows first and stays the reference for the dev comparison.
 */
#define HMRDP_IDWT_ROW_MAX 66

static INLINE void progressive_rfx_idwt_x_scalar(const INT16* WINPR_RESTRICT pLowBand,
                                                 size_t nLowStep,
                                                 const INT16* WINPR_RESTRICT pHighBand,
                                                 size_t nHighStep, INT16* WINPR_RESTRICT pDstBand,
                                                 size_t nDstStep, size_t nLowCount,
                                                 size_t nHighCount, size_t nDstCount)
{
	INT16 L0 = 0;
	INT16 H0 = 0;
	INT16 H1 = 0;
	INT16 X0 = 0;
	INT16 X1 = 0;
	INT16 X2 = 0;

	for (size_t i = 0; i < nDstCount; i++)
	{
		const INT16* pL = pLowBand;
		const INT16* pH = pHighBand;
		INT16* pX = pDstBand;
		H0 = *pH++;
		L0 = *pL++;
		X0 = L0 - H0;
		X2 = L0 - H0;

		for (size_t j = 0; j < (nHighCount - 1); j++)
		{
			H1 = *pH;
			pH++;
			L0 = *pL;
			pL++;
			X2 = L0 - ((H0 + H1) / 2);
			X1 = ((X0 + X2) / 2) + (2 * H0);
			pX[0] = X0;
			pX[1] = X1;
			pX += 2;
			X0 = X2;
			H0 = H1;
		}

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				pX[0] = X2;
				pX[1] = X2 + (2 * H0);
			}
			else
			{
				L0 = *pL;
				pL++;
				X0 = L0 - H0;
				pX[0] = X2;
				pX[1] = ((X0 + X2) / 2) + (2 * H0);
				pX[2] = X0;
			}
		}
		else
		{
			L0 = *pL;
			pL++;
			X0 = L0 - (H0 / 2);
			pX[0] = X2;
			pX[1] = ((X0 + X2) / 2) + (2 * H0);
			pX[2] = X0;
			L0 = *pL;
			pL++;
			pX[3] = (X0 + L0) / 2;
		}

		pLowBand += nLowStep;
		pHighBand += nHighStep;
		pDstBand += nDstStep;
	}
}

static INLINE void progressive_rfx_idwt_y_scalar(const INT16* WINPR_RESTRICT pLowBand,
                                                 size_t nLowStep,
                                                 const INT16* WINPR_RESTRICT pHighBand,
                                                 size_t nHighStep, INT16* WINPR_RESTRICT pDstBand,
                                                 size_t nDstStep, size_t nLowCount,
                                                 size_t nHighCount, size_t nDstCount)
{
	INT16 L0 = 0;
	INT16 H0 = 0;
	INT16 H1 = 0;
	INT16 X0 = 0;
	INT16 X1 = 0;
	INT16 X2 = 0;

	for (size_t i = 0; i < nDstCount; i++)
	{
		const INT16* pL = pLowBand;
		const INT16* pH = pHighBand;
		INT16* pX = pDstBand;
		H0 = *pH;
		pH += nHighStep;
		L0 = *pL;
		pL += nLowStep;
		X0 = L0 - H0;
		X2 = L0 - H0;

		for (size_t j = 0; j < (nHighCount - 1); j++)
		{
			H1 = *pH;
			pH += nHighStep;
			L0 = *pL;
			pL += nLowStep;
			X2 = L0 - ((H0 + H1) / 2);
			X1 = ((X0 + X2) / 2) + (2 * H0);
			*pX = X0;
			pX += nDstStep;
			*pX = X1;
			pX += nDstStep;
			X0 = X2;
			H0 = H1;
		}

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				*pX = X2;
				pX += nDstStep;
				*pX = X2 + (2 * H0);
			}
			else
			{
				L0 = *pL;
				X0 = L0 - H0;
				*pX = X2;
				pX += nDstStep;
				*pX = ((X0 + X2) / 2) + (2 * H0);
				pX += nDstStep;
				*pX = X0;
			}
		}
		else
		{
			L0 = *pL;
			pL += nLowStep;
			X0 = L0 - (H0 / 2);
			*pX = X2;
			pX += nDstStep;
			*pX = ((X0 + X2) / 2) + (2 * H0);
			pX += nDstStep;
			*pX = X0;
			pX += nDstStep;
			L0 = *pL;
			*pX = (X0 + L0) / 2;
		}

		pLowBand++;
		pHighBand++;
		pDstBand++;
	}
}

static INLINE void progressive_rfx_idwt_x(const INT16* WINPR_RESTRICT pLowBand, size_t nLowStep,
                                          const INT16* WINPR_RESTRICT pHighBand, size_t nHighStep,
                                          INT16* WINPR_RESTRICT pDstBand, size_t nDstStep,
                                          size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
	/* X0 at step j (the even sample written at 2j) is the X2 of step j - 1, so
	 * the whole X2 sequence is one dependency-free run over the bands; the pairs
	 * are then written by a second run. */
	INT16 x0[HMRDP_IDWT_ROW_MAX + 1];
	size_t i = 0;
	size_t j = 0;

	for (i = 0; i < nDstCount; i++)
	{
		const INT16* WINPR_RESTRICT pL = pLowBand;
		const INT16* WINPR_RESTRICT pH = pHighBand;
		INT16* WINPR_RESTRICT pX = pDstBand;

		x0[0] = (INT16)(pL[0] - pH[0]);
		for (j = 0; j + 1 < nHighCount; j++)
			x0[j + 1] = (INT16)(pL[j + 1] - (((int)pH[j] + (int)pH[j + 1]) / 2));

		for (j = 0; j + 1 < nHighCount; j++)
		{
			pX[0] = x0[j];
			pX[1] = (INT16)((((int)x0[j] + (int)x0[j + 1]) / 2) + (2 * (int)pH[j]));
			pX += 2;
		}

		/* The tail keeps the upstream cases, with X2 = x0[nHighCount - 1] and
		 * H0 = pH[nHighCount - 1] (what the scalar loop leaves behind). */
		{
			const INT16 H0 = pH[nHighCount - 1];
			const INT16 X2 = x0[nHighCount - 1];

			if (nLowCount <= (nHighCount + 1))
			{
				if (nLowCount <= nHighCount)
				{
					pX[0] = X2;
					pX[1] = (INT16)(X2 + (2 * (int)H0));
				}
				else
				{
					const INT16 X0 = (INT16)(pL[nHighCount] - H0);
					pX[0] = X2;
					pX[1] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)H0));
					pX[2] = X0;
				}
			}
			else
			{
				const INT16 X0 = (INT16)(pL[nHighCount] - (H0 / 2));
				pX[0] = X2;
				pX[1] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)H0));
				pX[2] = X0;
				pX[3] = (INT16)(((int)X0 + (int)pL[nHighCount + 1]) / 2);
			}
		}

		pLowBand += nLowStep;
		pHighBand += nHighStep;
		pDstBand += nDstStep;
	}
}

static INLINE void progressive_rfx_idwt_y(const INT16* WINPR_RESTRICT pLowBand, size_t nLowStep,
                                          const INT16* WINPR_RESTRICT pHighBand, size_t nHighStep,
                                          INT16* WINPR_RESTRICT pDstBand, size_t nDstStep,
                                          size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
	/* Row-major: the band row is the outer loop and the tile columns are the
	 * inner one, so every access is a contiguous run. The recurrence only needs
	 * the previous X2 row, which the swap keeps. */
	INT16 rowA[HMRDP_IDWT_ROW_MAX + 1];
	INT16 rowB[HMRDP_IDWT_ROW_MAX + 1];
	INT16* WINPR_RESTRICT prev = rowA;
	INT16* WINPR_RESTRICT cur = rowB;
	size_t i = 0;
	size_t j = 0;

	for (i = 0; i < nDstCount; i++)
		prev[i] = (INT16)(pLowBand[i] - pHighBand[i]);

	for (j = 0; j + 1 < nHighCount; j++)
	{
		const INT16* WINPR_RESTRICT lj = pLowBand + (j + 1) * nLowStep;
		const INT16* WINPR_RESTRICT hj = pHighBand + j * nHighStep;
		INT16* WINPR_RESTRICT o0 = pDstBand + (j * 2) * nDstStep;
		INT16* WINPR_RESTRICT o1 = o0 + nDstStep;

		for (i = 0; i < nDstCount; i++)
			cur[i] = (INT16)(lj[i] - (((int)hj[i] + (int)hj[nHighStep + i]) / 2));

		for (i = 0; i < nDstCount; i++)
		{
			o0[i] = prev[i];
			o1[i] = (INT16)((((int)prev[i] + (int)cur[i]) / 2) + (2 * (int)hj[i]));
		}

		{
			INT16* WINPR_RESTRICT swap = prev;
			prev = cur;
			cur = swap;
		}
	}

	/* Tail: X2 = prev, H0 = the last high-band row. */
	{
		const INT16* WINPR_RESTRICT h0 = pHighBand + (nHighCount - 1) * nHighStep;
		INT16* WINPR_RESTRICT o = pDstBand + (2 * (nHighCount - 1)) * nDstStep;

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				for (i = 0; i < nDstCount; i++)
				{
					o[i] = prev[i];
					o[nDstStep + i] = (INT16)(prev[i] + (2 * (int)h0[i]));
				}
			}
			else
			{
				const INT16* WINPR_RESTRICT ln = pLowBand + nHighCount * nLowStep;
				for (i = 0; i < nDstCount; i++)
				{
					const INT16 X0 = (INT16)(ln[i] - h0[i]);
					const INT16 X2 = prev[i];
					o[i] = X2;
					o[nDstStep + i] =
					    (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)h0[i]));
					o[2 * nDstStep + i] = X0;
				}
			}
		}
		else
		{
			const INT16* WINPR_RESTRICT ln = pLowBand + nHighCount * nLowStep;
			for (i = 0; i < nDstCount; i++)
			{
				const INT16 X0 = (INT16)(ln[i] - (h0[i] / 2));
				const INT16 X2 = prev[i];
				const INT16 L1 = ln[nLowStep + i];
				o[i] = X2;
				o[nDstStep + i] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)h0[i]));
				o[2 * nDstStep + i] = X0;
				o[3 * nDstStep + i] = (INT16)(((int)X0 + (int)L1) / 2);
			}
		}
	}
}

static INLINE size_t progressive_rfx_get_band_l_count(size_t level)
{
	return (64 >> level) + 1;
}

static INLINE size_t progressive_rfx_get_band_h_count(size_t level)
{
	if (level == 1)
		return (64 >> 1) - 1;
	else
		return (64 + (1 << (level - 1))) >> level;
}

'@

# ... the block wrapper learns a "scalar" switch (used by the dev comparison).
$progDwtBlock = @'
static INLINE void progressive_rfx_dwt_2d_decode_block_mode(INT16* WINPR_RESTRICT buffer,
                                                            INT16* WINPR_RESTRICT temp, size_t level,
                                                            BOOL scalar)
{
	size_t nDstStepX = 0;
	size_t nDstStepY = 0;
	const INT16* WINPR_RESTRICT HL = NULL;
	const INT16* WINPR_RESTRICT LH = NULL;
	const INT16* WINPR_RESTRICT HH = NULL;
	INT16* WINPR_RESTRICT LL = NULL;
	INT16* WINPR_RESTRICT L = NULL;
	INT16* WINPR_RESTRICT H = NULL;
	INT16* WINPR_RESTRICT LLx = NULL;

	const size_t nBandL = progressive_rfx_get_band_l_count(level);
	const size_t nBandH = progressive_rfx_get_band_h_count(level);
	size_t offset = 0;

	HL = &buffer[offset];
	offset += (nBandH * nBandL);
	LH = &buffer[offset];
	offset += (nBandL * nBandH);
	HH = &buffer[offset];
	offset += (nBandH * nBandH);
	LL = &buffer[offset];
	nDstStepX = (nBandL + nBandH);
	nDstStepY = (nBandL + nBandH);
	offset = 0;
	L = &temp[offset];
	offset += (nBandL * nDstStepX);
	H = &temp[offset];
	LLx = &buffer[0];

	if (scalar)
	{
		/* horizontal (LL + HL -> L) */
		progressive_rfx_idwt_x_scalar(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);

		/* horizontal (LH + HH -> H) */
		progressive_rfx_idwt_x_scalar(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);

		/* vertical (L + H -> LL) */
		progressive_rfx_idwt_y_scalar(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH,
		                              nBandL + nBandH);
		return;
	}

	/* horizontal (LL + HL -> L) */
	progressive_rfx_idwt_x(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);

	/* horizontal (LH + HH -> H) */
	progressive_rfx_idwt_x(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);

	/* vertical (L + H -> LL) */
	progressive_rfx_idwt_y(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH,
	                       nBandL + nBandH);
}

static INLINE void progressive_rfx_dwt_2d_decode_block(INT16* WINPR_RESTRICT buffer,
                                                       INT16* WINPR_RESTRICT temp, size_t level)
{
	progressive_rfx_dwt_2d_decode_block_mode(buffer, temp, level, FALSE);
}

'@

# ... and the entry point runs the dev comparison on a sample of tiles.
$progDwtExtrapolate = @'
/* Exported by the patched rfx_dwt.c (step 15): the dev comparison of the
 * inverse DWT against its scalar reference. Armed for one tile in 16 of the
 * reference runs - the reduced DWT is the one a Progressive region actually
 * uses, so this is where the sample has to be taken. */
extern int HmrdpDwtCheckArmed(void);
extern void HmrdpDwtCheckResult(unsigned int bad);

static void hmrdp_dwt_2d_decode_extrapolate_mode(INT16* WINPR_RESTRICT buffer,
                                                 INT16* WINPR_RESTRICT temp, BOOL scalar)
{
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[3807], temp, 3, scalar);
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[3007], temp, 2, scalar);
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[0], temp, 1, scalar);
}

void rfx_dwt_2d_extrapolate_decode(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT temp)
{
	static INT16 ref[4096] = { 0 };
	static INT16 scratch[4096] = { 0 };
	const BOOL check = HmrdpDwtCheckArmed();

	WINPR_ASSERT(buffer);
	WINPR_ASSERT(temp);

	/* HmRdp dev: the decode overwrites the whole coefficient buffer, so the input
	 * is copied first and the scalar reference runs on that copy below. */
	if (check)
		memcpy(ref, buffer, sizeof(ref));

	hmrdp_dwt_2d_decode_extrapolate_mode(buffer, temp, FALSE);

	if (check)
	{
		UINT32 bad = 0;
		size_t i = 0;

		hmrdp_dwt_2d_decode_extrapolate_mode(ref, scratch, TRUE);

		for (i = 0; i < 4096; i++)
		{
			if (ref[i] != buffer[i])
				bad++;
		}
		HmrdpDwtCheckResult(bad);
	}
}
'@

# One replacement for the whole region: the `.*?` between the anchors absorbs
# whatever version is in the tree, so re-running over an already patched source
# stays safe (the marker below is what makes it a no-op then).
Patch-Regex $progDwtC 'static INLINE void progressive_rfx_idwt_x\(const INT16\* WINPR_RESTRICT pLowBand.*?\nvoid rfx_dwt_2d_extrapolate_decode\(INT16\* WINPR_RESTRICT buffer, INT16\* WINPR_RESTRICT temp\)\n\{.*?\n\}\n' ($progDwtRows + $progDwtBlock + $progDwtExtrapolate) 'hmrdp_dwt_2d_decode_extrapolate_mode'

Write-Host "FreeRDP OHOS patches applied to $Source"

