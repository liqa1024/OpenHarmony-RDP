# 11) HmRdp: the decode width comes from the app; the serial/parallel choice
#     follows it.
#
#     背景：多核执行器已统一到平台的 ffrt 并发队列（hmrdp_parallel.*，见 step 21）。
#     并行宽度是 app 的一个进程级值，app 通过 `HmrdpDecodeWidth()` 暴露；解码器只
#     需要据此在"接收线程串行"与"提交平台队列"之间选择。这里不再有任何 WinPR 池
#     相关接线（宽度不再是"池的 worker 数"）。
#
#     (a) 解码器读 app 导出的 `HmrdpDecodeWidth()`（弱符号；没有平台执行器的构建
#         返回 1 ⇒ 串行）。
#     (b) 串行分支的判据是宽度 <= 1（rfx 的 UseThreads 属于旧的池开关，不再参与）。
#     (c) 保留解码相位计时与 HmrdpProgStat（dev 探针，另有开关见 step 24）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progC = "$Source\libfreerdp\codec\progressive.c"

# (a) the app-provided width, next to the tile-chunk helpers.
Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/* HmRdp: the decode width requested by the app (hmrdp_parallel.* / the settings
 * knob). Weak, so a build without the platform executor decodes on the receiving
 * thread. */
extern unsigned int HmrdpDecodeWidth(void) __attribute__((weak));

static INLINE UINT32 hmrdp_decode_width(void)
{
	return HmrdpDecodeWidth != NULL ? HmrdpDecodeWidth() : 1u;
}

static INLINE SSIZE_T progressive_process_tiles(
'@) 'hmrdp_decode_width'

# (b) the serial branch: width 1 (or no platform executor) decodes on the
#     receiving thread, with no task submission. Time it too: without the timers
#     the serial branch would only show the parse (`read`) and `update_tiles`
#     costs, and the `prog` line would look like the decode is nearly free.
Patch-Regex $progC '\tif \(!progressive->rfx_context->priv->UseThreads\)\n\t\{\n\t\t/\* Serial: one call per tile, exactly as before the chunking change\. \*/\n\t\tfor \(UINT32 idx = 0; idx < region->numTiles; idx\+\+\)\n\t\t\tprogressive_process_tiles_tile_work_callback\(0, &progressive->params\[idx\], 0\);\n\n\t\tgoto fail;\n\t\}\n' (@'
	if (hmrdp_decode_width() <= 1)
	{
		/* Serial (width 1, or no platform executor): one call per tile, no task
		 * submission. HmRdp dev: the tiles and the time spent decoding them are
		 * counted here - the queue-path counters stay 0 on this branch. */
		const unsigned long long s0 = hmrdp_now_ns();
		for (UINT32 idx = 0; idx < region->numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);

		__atomic_add_fetch(&HmrdpProgStat[8], region->numTiles, __ATOMIC_RELAXED);
		__atomic_add_fetch(&HmrdpProgStat[9], hmrdp_now_ns() - s0, __ATOMIC_RELAXED);
		goto fail;
	}
'@ + "`n") 'the tiles and the time spent decoding them are counted'

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
 * clock reads stay out of the figures they report. The per-tile flag is thread
 * local, so it is exact on the queue path too.
 */
static UINT32 g_HmrdpTileSample = 0;
/* HmRdp: per thread. The flag decides whether *this* tile's phases are timed; a
 * shared flag would leak one worker's sampling decision into the others and make
 * the per-phase totals meaningless as soon as the decode runs on more than one
 * thread. A tile decode never yields, so the flag is stable for the whole tile
 * even though the platform queue's tasks can migrate between workers. */
static _Thread_local BOOL g_HmrdpSampleTile = FALSE;
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

Patch-Regex $progC '\t                                    &roi_64x64\);\nfail:\n\tBufferPool_Return\(progressive->bufferPool, pBuffer\);\n\treturn rc;\n\}\n' (@'
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

