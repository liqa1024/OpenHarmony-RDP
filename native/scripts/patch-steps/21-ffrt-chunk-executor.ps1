# 21) HmRdp: run the tile-decode chunks on the platform task queue (ffrt).
#
#     动机（doc_agent/cpu-accel-plan.md §0/§2）：现在这套并行用的是 FreeRDP
#     自己的 WinPR 池——每个 codec 上下文自带一组线程、每个 chunk 一个 work item
#     进带锁队列、等待走池级全局计数。实测它的代价是"把同一份解码放大成约 4.6 倍
#     CPU"（整轮进程 CPU 约 3 倍、帧墙钟只降 1.5 倍），而且请求 2/4/8 个 worker
#     得到的 CPU 与墙钟完全一样：请求的宽度没有变成有效并行宽度。
#
#     这一步**只换执行器**：chunk 还是那些 chunk（同样的划分、同样的共享领取计数、
#     同样的 per-chunk scratch），只是由 ffrt 提交/调度/等待，不再建 WinPR 池。
#     这样 A/B 的差异只归因于执行器本身。
#
#     绑定方式是**弱符号**：`HmrdpParallelAvailable/Run` 由 app（hmrdp_parallel.*）
#     提供，未打过补丁的 FreeRDP 或没有该模块的构建自动走原来的池分支（
#     native-libraries.md §3 的既有约定）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progParC = "$Source\libfreerdp\codec\progressive.c"

# (a) the platform executor, next to the app-width helper inserted by step 11.
Patch-Regex $progParC `
  '/\* HmRdp: the decode width requested by the app \(hmrdp_parallel\.\* / the settings\n \* knob\)\. Weak, so a build without the platform executor decodes on the receiving\n \* thread\. \*/\nextern unsigned int HmrdpDecodeWidth\(void\) __attribute__\(\(weak\)\);\n\nstatic INLINE UINT32 hmrdp_decode_width\(void\)\n\{\n\treturn HmrdpDecodeWidth != NULL \? HmrdpDecodeWidth\(\) : 1u;\n\}' (@'
/* HmRdp: the decode width requested by the app (hmrdp_parallel.* / the settings
 * knob). Weak, so a build without the platform executor decodes on the receiving
 * thread. */
extern unsigned int HmrdpDecodeWidth(void) __attribute__((weak));

static INLINE UINT32 hmrdp_decode_width(void)
{
	return HmrdpDecodeWidth != NULL ? HmrdpDecodeWidth() : 1u;
}

/* HmRdp: the platform task queue (ffrt), exported by the app
 * (hmrdp_parallel.*). Both stay unresolved on a build without it; the width
 * helper above then forces the serial branch. */
extern int HmrdpParallelAvailable(void) __attribute__((weak));
extern int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx)
    __attribute__((weak));

/* Tasks one region is split into when ffrt runs it. A region carries only a few
 * milliseconds of work, so this is deliberately far below HMRDP_TILE_CHUNKS: the
 * platform queue charges per submitted task. */
#define HMRDP_FFRT_TASKS 16
'@) 'the platform task queue (ffrt)'

# (b) the ffrt task body: the same chunk callback the tile work uses, so the
#     executor swap changes nothing about the work.
Patch-Regex $progParC `
  '\t__atomic_add_fetch\(&HmrdpProgStat\[9\], hmrdp_now_ns\(\) - c0, __ATOMIC_RELAXED\);\n\}\n\n/\* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above\. \*/' (@'
	__atomic_add_fetch(&HmrdpProgStat[9], hmrdp_now_ns() - c0, __ATOMIC_RELAXED);
}

/* HmRdp: one ffrt task per chunk. It forwards to the very same chunk callback the
 * tile work uses, so the executor swap changes nothing about the work. */
static void hmrdp_chunk_ffrt_callback(void* ctx, unsigned int index)
{
	PROGRESSIVE_TILE_CHUNK_PARAM* chunks = (PROGRESSIVE_TILE_CHUNK_PARAM*)ctx;

	progressive_process_tile_chunk_callback((PTP_CALLBACK_INSTANCE)0, (void*)&chunks[index],
	                                        (PTP_WORK)0);
}

/* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above. */
'@) 'hmrdp_chunk_ffrt_callback'

# (c) the parallel branch is the platform queue, full stop: the WinPR pool path
#     is deleted. A build without the queue never reaches here (the width helper
#     forces the serial branch), but the fallback keeps the region
#     all-or-nothing regardless.
Patch-Regex $progParC `
  '\t\tUINT32 numChunks = HMRDP_TILE_CHUNKS;.*?\n\t\tHmrdpProgStat\[2\] \+= hmrdp_now_ns\(\) - ht2; /\* wait\+close \(serial\) \*/\n' (@'
		UINT32 numChunks = HMRDP_FFRT_TASKS;
		if (numChunks > numTiles)
			numChunks = numTiles;

		if (HmrdpParallelAvailable != NULL && HmrdpParallelRun != NULL &&
		    (HmrdpParallelAvailable() != 0))
		{
			/* HmRdp: one ffrt task per chunk - see the patch note in
			 * native/scripts/patch-freerdp.ps1 step 21. A region carries only a few
			 * ms of work, so handing ffrt one task per chunk pays more in task
			 * objects than it buys in scheduling. */
			for (UINT32 c = 0; c < numChunks; c++)
			{
				PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
				chunk->params = progressive->params;
				chunk->scratch =
				    progressive->tileScratch + ((size_t)c * (size_t)HMRDP_TILE_SCRATCH_STRIDE);
				chunk->next = &nextTile;
				chunk->numTiles = numTiles;
			}

			HmrdpProgStat[1] += hmrdp_now_ns() - ht1; /* dispatch (serial) */
			/* Dev: proves the decode ran on the platform queue. */
			HmrdpProgStat[18] += 1;
			{
				const unsigned long long ht2 = hmrdp_now_ns();
				(void)HmrdpParallelRun(numChunks, hmrdp_chunk_ffrt_callback, (void*)chunks);
				HmrdpProgStat[2] += hmrdp_now_ns() - ht2; /* wait (serial) */
			}
			goto fail;
		}

		/* No platform queue in this build: decode on the receiving thread. The
		 * width helper already forces the serial branch then, so this is only
		 * belt and braces - the region stays all-or-nothing. */
		g_HmrdpTlsTileScratch = progressive->tileScratch;
		HmrdpProgStat[1] += hmrdp_now_ns() - ht1; /* dispatch (serial) */
		{
			const unsigned long long ht2 = hmrdp_now_ns();
			for (UINT32 idx = 0; idx < numTiles; idx++)
				progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);
			__atomic_add_fetch(&HmrdpProgStat[8], numTiles, __ATOMIC_RELAXED);
			HmrdpProgStat[2] += hmrdp_now_ns() - ht2; /* wait (serial) */
		}
'@ + "`n") 'never reaches here'
