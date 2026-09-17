# 21) HmRdp: run the tile-decode chunks on the platform task queue (ffrt).
#
#     动机（doc_agent/cpu-accel-plan.md §1/§2 M-a）：现在这套并行用的是 FreeRDP
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

# (a) the optional platform executor, next to the existing winpr hooks.
Patch-Regex $progParC `
  '/\* Exported by the patched libwinpr: the worker count the app asked for, and the\n \* point where it is safe to resize the pool \(no work item in flight yet\)\. \*/\nextern DWORD HmrdpGetDecodeThreads\(void\);\nextern void HmrdpApplyDecodeThreads\(PTP_POOL pool\);' (@'
/* Exported by the patched libwinpr: the worker count the app asked for, and the
 * point where it is safe to resize the pool (no work item in flight yet). */
extern DWORD HmrdpGetDecodeThreads(void);
extern void HmrdpApplyDecodeThreads(PTP_POOL pool);

/* HmRdp: the optional platform task queue (ffrt), exported by the app
 * (hmrdp_parallel.*). Both stay unresolved on a build without it, and the
 * executor falls back to the codec's own WinPR pool. */
extern int HmrdpParallelAvailable(void) __attribute__((weak));
extern int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx)
    __attribute__((weak));

/* Tasks one region is split into when ffrt runs it. A region carries only a few
 * milliseconds of work, so this is deliberately below HMRDP_TILE_CHUNKS: the
 * platform queue charges per submitted task, and 64 tasks per region measured
 * slower than the pool it replaces. */
#define HMRDP_FFRT_TASKS 16
'@) 'HmrdpParallelAvailable'

# (b) the ffrt task body: the same chunk callback the pool path uses, so the two
#     executors run identical work.
Patch-Regex $progParC `
  '\t__atomic_add_fetch\(&HmrdpProgStat\[9\], hmrdp_now_ns\(\) - c0, __ATOMIC_RELAXED\);\n\}\n\n/\* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above\. \*/' (@'
	__atomic_add_fetch(&HmrdpProgStat[9], hmrdp_now_ns() - c0, __ATOMIC_RELAXED);
}

/* HmRdp: one ffrt task per chunk. It forwards to the very same chunk callback the
 * WinPR path uses, so switching executors changes nothing about the work. */
static void hmrdp_chunk_ffrt_callback(void* ctx, unsigned int index)
{
	PROGRESSIVE_TILE_CHUNK_PARAM* chunks = (PROGRESSIVE_TILE_CHUNK_PARAM*)ctx;

	progressive_process_tile_chunk_callback((PTP_CALLBACK_INSTANCE)0, (void*)&chunks[index],
	                                        (PTP_WORK)0);
}

/* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above. */
'@) 'hmrdp_chunk_ffrt_callback'

# (c) take the platform path when it is there, before any WinPR work item is made.
Patch-Regex $progParC `
  '\t\tfor \(UINT32 c = 0; c < numChunks; c\+\+\)\n\t\t\{\n\t\t\tPROGRESSIVE_TILE_CHUNK_PARAM\* chunk = &chunks\[c\];\n\t\t\tchunk->params = progressive->params;\n\t\t\tchunk->scratch =\n\t\t\t    progressive->tileScratch \+ \(\(size_t\)c \* \(size_t\)HMRDP_TILE_SCRATCH_STRIDE\);\n\t\t\tchunk->next = &nextTile;\n\t\t\tchunk->numTiles = numTiles;\n\n\t\t\tprogressive->work_objects\[c\] =' (@'
		if (HmrdpParallelAvailable != NULL && HmrdpParallelRun != NULL &&
		    (HmrdpParallelAvailable() != 0))
		{
			/* HmRdp: run the chunks on the platform task queue instead of the
			 * codec's own pool - see the patch note in native/scripts/patch-freerdp.ps1
			 * step 21. Each chunk gets its own tile range and its own claim counter,
			 * so no two tasks share a cache line; the pool path's single shared
			 * counter is a serialisation point that width cannot hide. */
			volatile UINT32 ffrtClaims[HMRDP_TILE_CHUNKS];
			UINT32 ffrtTasks = HMRDP_FFRT_TASKS;
			if (ffrtTasks > numTiles)
				ffrtTasks = numTiles;

			for (UINT32 c = 0; c < ffrtTasks; c++)
			{
				PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
				chunk->params = progressive->params;
				chunk->scratch =
				    progressive->tileScratch + ((size_t)c * (size_t)HMRDP_TILE_SCRATCH_STRIDE);
				chunk->next = &ffrtClaims[c];
				ffrtClaims[c] = (UINT32)(((UINT64)c * numTiles) / ffrtTasks);
				chunk->numTiles = (UINT32)(((UINT64)(c + 1u) * numTiles) / ffrtTasks);
			}

			HmrdpProgStat[1] += hmrdp_now_ns() - ht1; /* dispatch (serial) */
			/* Dev: proves which executor ran (HmrdpProgStat[18] is otherwise unused). */
			HmrdpProgStat[18] += 1;
			{
				const unsigned long long ht2 = hmrdp_now_ns();
				(void)HmrdpParallelRun(ffrtTasks, hmrdp_chunk_ffrt_callback, (void*)chunks);
				HmrdpProgStat[2] += hmrdp_now_ns() - ht2; /* wait (serial) */
			}
			goto fail;
		}

		for (UINT32 c = 0; c < numChunks; c++)
		{
			PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
			chunk->params = progressive->params;
			chunk->scratch =
			    progressive->tileScratch + ((size_t)c * (size_t)HMRDP_TILE_SCRATCH_STRIDE);
			chunk->next = &nextTile;
			chunk->numTiles = numTiles;

			progressive->work_objects[c] =
'@) 'the platform task queue instead of the'
