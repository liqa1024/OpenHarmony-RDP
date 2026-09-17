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

