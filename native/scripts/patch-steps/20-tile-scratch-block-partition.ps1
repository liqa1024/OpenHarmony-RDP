# 20) HmRdp: take the tile decode's working buffers out of the shared pool, and
#     claim tiles in blocks instead of one at a time.
#
#     背景（doc_agent/cpu-accel-plan.md §2）：改之前并行解码在 2 个 worker
#     之后完全不再涨，而且整轮进程 CPU 是单核的 3 倍以上。两个机制性原因都在
#     "所有 worker 争同一份共享结构"上：
#
#       a) `progressive->bufferPool` 是**带临界区的全局池**，而 tile 解码每个
#          分量都要 Take/Return 一次 DWT 临时缓冲，加上 tile 自己的 24 KB 工作
#          缓冲 —— 每个 tile 共 8 次临界区进出（3 分量 ×2 + 2），整屏帧上万次/
#          帧；池的空闲链表头又是所有 worker 共享写的一条 cache line。
#       b) 分片投递用一个 `volatile UINT32 nextTile` 动态领取 tile，计数器在 RDP
#          线程的栈上，所有 worker 用 `__sync_fetch_and_add` 争这条 cache line。
#
#     这一步两处都换掉，**不改变任何像素**（缓冲最终内容一样、tile 集合一样、
#     合成区域一样），只改"谁用哪块内存、工作怎么分"：
#
#       a) 每个 chunk 独占一个 2 缓冲的 scratch 槽（工作缓冲 + DWT 临时），随
#          context 一次性分配、永不在 tile 上 Take/Return。当前槽是线程本地变量
#          （chunk 回调设置，串行分支用 0 号槽），所以 tile 解码的函数签名保持
#          上游原样，只换分配调用。
#       b) 领取计数器保留（动态领取是"负载均衡"的来源：tile 成本不齐，静态区间
#          在 2 worker 下实测把解码段从 11.8ms 拉到 27.6ms），但**按块领取**
#          （HMRDP_TILE_CLAIM）：共享计数器的流量降一个数量级，代价最多是尾块
#          几 tile 的不均衡；计数器本身 64 字节对齐，不与 chunk 描述符同一条
#          cache line。work item 数量**维持原来的每块 1 个**（不按 worker 数收缩）：
#          本平台上收缩到 2×worker 会让池的第二个线程不参与（2 worker + 4 个
#          work item 的解码段与串行一样长）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progScratchH = "$Source\libfreerdp\codec\progressive.h"
$progScratchC = "$Source\libfreerdp\codec\progressive.c"

# (a0) the per-context scratch arena pointer.
Patch-Regex $progScratchH `
  '\tPROGRESSIVE_TILE_PROCESS_WORK_PARAM params\[0x10000\];\n\tPTP_WORK work_objects\[0x10000\];\n\};' (@'
	PROGRESSIVE_TILE_PROCESS_WORK_PARAM params[0x10000];
	PTP_WORK work_objects[0x10000];
	/* HmRdp: one tile working-buffer slot per dispatched chunk (progressive.c's
	 * hmrdp_tile_scratch), allocated once per context so the tile decode takes no
	 * lock and no shared buffer-pool entry. */
	BYTE* tileScratch;
};
'@) 'BYTE* tileScratch;'

# (a1) the scratch helpers, near the top of the file (they are used by the tile
#      callback below).
Patch-Regex $progScratchC `
  '#define TAG FREERDP_TAG\("codec\.progressive"\)\n' (@'
#define TAG FREERDP_TAG("codec.progressive")

/* HmRdp: per-chunk tile working buffers.
 *
 * One tile decode needs two 24 KB working buffers (the coefficient working
 * buffer and the inverse-DWT scratch). Upstream takes both from
 * `progressive->bufferPool` inside every tile and every component, i.e. eight
 * lock round trips per tile (a Take and a Return per component plus the tile's
 * own buffer). On a full-screen frame that is thousands of acquisitions of a
 * data structure whose free-list head is a cache line shared by every worker -
 * the main reason the pool does not scale past two workers.
 *
 * Instead each dispatched chunk owns one two-buffer slot, allocated once per
 * context and never taken per tile: no lock and no shared cache line in the
 * tile decode. The slot in use is a thread-local set by the chunk callback (the
 * serial branch uses slot 0), so the tile decode keeps its upstream signatures
 * and only the allocator calls change. The bytes each buffer ends up holding are
 * unchanged.
 */
#define HMRDP_TILE_CHUNKS 64
#define HMRDP_TILE_CLAIM 2
#define HMRDP_TILE_SCRATCH_BYTES ((8192ULL + 32ULL) * 3ULL)
#define HMRDP_TILE_SCRATCH_STRIDE (HMRDP_TILE_SCRATCH_BYTES * 2ULL)

static _Thread_local BYTE* g_HmrdpTlsTileScratch = NULL;

/* The working-buffer slot of the chunk this thread is decoding, or slot 0 when
 * called outside the chunk dispatch (the serial branch). */
static INLINE BYTE* hmrdp_tile_scratch(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive)
{
	if (g_HmrdpTlsTileScratch)
		return g_HmrdpTlsTileScratch;
	return progressive->tileScratch;
}

/* The inverse-DWT scratch of the current slot. */
static INLINE INT16* hmrdp_tile_dwt_scratch(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive)
{
	return (INT16*)(void*)(hmrdp_tile_scratch(progressive) + HMRDP_TILE_SCRATCH_BYTES);
}

/* One slot per chunk, one allocation per context; released with the context. */
static INLINE BOOL hmrdp_alloc_tile_scratch(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive)
{
	BYTE* scratch = (BYTE*)winpr_aligned_malloc(
	    (size_t)HMRDP_TILE_CHUNKS * (size_t)HMRDP_TILE_SCRATCH_STRIDE, 64);
	if (!scratch)
		return FALSE;
	progressive->tileScratch = scratch;
	return TRUE;
}
'@) 'g_HmrdpTlsTileScratch'

# (a2) the old chunk-count define is now next to the scratch helpers.
Patch-Regex-All $progScratchC `
  '/\* Upper bound on the work items a single region may be split into\. \*/\n#define HMRDP_TILE_CHUNKS 64' `
  '/* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above. */' `
  'is defined with the tile scratch helpers'

# (a3) the DWT scratch comes from the current chunk's slot.
Patch-Regex $progScratchC `
  '\tINT16\* temp = \(INT16\*\)BufferPool_Take\(progressive->bufferPool, -1\); /\* DWT buffer \*/\n\n\tif \(!temp\)\n\t\treturn -2;\n' (@'
	/* HmRdp: the inverse-DWT scratch is the current chunk's second slot (see
	 * hmrdp_tile_scratch) - no pool Take/Return per component. */
	INT16* temp = hmrdp_tile_dwt_scratch(progressive);

'@) 'hmrdp_tile_dwt_scratch(progressive)'

Patch-Regex $progScratchC '\tBufferPool_Return\(progressive->bufferPool, temp\);\n\treturn 1;' (@'
	/* HmRdp: no pool Return for the DWT scratch - the slot is reused as is. */
	return 1;
'@) 'no pool Return for the DWT scratch'

# (a4) the tile working buffer comes from the current chunk's slot. Both the
#      FIRST and the UPGRADE tile path take it; neither returns it.
$tileScratchTake = @'
	pBuffer = hmrdp_tile_scratch(progressive); /* HmRdp: this chunk's slot */
'@
Patch-Regex-All $progScratchC `
  '\tpBuffer = \(BYTE\*\)BufferPool_Take\(progressive->bufferPool, -1\);' `
  $tileScratchTake `
  'pBuffer = hmrdp_tile_scratch(progressive);'

Patch-Regex-All $progScratchC `
  '\tBufferPool_Return\(progressive->bufferPool, pBuffer\);\n' (@'
	/* HmRdp: no pool Return for the tile buffer - the slot is reused as is. */
'@) 'no pool Return for the tile buffer'

# (b1) a chunk carries its own scratch slot and joins the shared claim cursor.
Patch-Regex $progScratchC `
  'typedef struct\n\{\n\tPROGRESSIVE_TILE_PROCESS_WORK_PARAM\* params;\n\tvolatile UINT32\* next;\n\tUINT32 numTiles;\n\} PROGRESSIVE_TILE_CHUNK_PARAM;' (@'
typedef struct
{
	PROGRESSIVE_TILE_PROCESS_WORK_PARAM* params;
	/* HmRdp: the chunk's own working-buffer slot (see hmrdp_tile_scratch). */
	BYTE* scratch;
	/* HmRdp: each chunk owns one contiguous *home* range of tiles - a worker walks
	 * contiguous memory and the different homes are far apart - and once its home
	 * is done it steals the other homes' remaining HMRDP_TILE_CLAIM blocks, so the
	 * tail is one block instead of one whole range. */
	volatile UINT32* homeNext;
	const UINT32* homeLimits;
	UINT32 homeCount;
	UINT32 homeIndex;
} PROGRESSIVE_TILE_CHUNK_PARAM;
'@) "the chunk's own working-buffer slot"

# (b2) the callback claims blocks from the shared cursor, with its own slot.
Patch-Regex $progScratchC `
  '\tfor \(;;\)\n\t\{\n\t\tconst UINT32 index = __sync_fetch_and_add\(chunk->next, 1u\);\n\t\tif \(index >= chunk->numTiles\)\n\t\t\tbreak;\n\n\t\tprogressive_process_tiles_tile_work_callback\(instance, &chunk->params\[index\], work\);\n\t\}' (@'
	/* HmRdp: this thread decodes only this chunk, so its working buffers are the
	 * chunk's slot - see hmrdp_tile_scratch(). */
	g_HmrdpTlsTileScratch = chunk->scratch;

	/* HmRdp: home range first (contiguous, this worker's own), then steal the
	 * other homes' remaining blocks in order - the tail is one block, while a
	 * worker's normal path stays on one contiguous range. Tiles are claimed in
	 * HMRDP_TILE_CLAIM blocks, which keeps the shared counter off the per-tile
	 * path; the cost is an imbalance of at most one block at the end. */
	for (UINT32 round = 0; round < chunk->homeCount; round++)
	{
		const UINT32 home = (chunk->homeIndex + round) % chunk->homeCount;
		volatile UINT32* cursor = &chunk->homeNext[home];
		const UINT32 limit = chunk->homeLimits[home];

		for (;;)
		{
			const UINT32 begin = __sync_fetch_and_add(cursor, HMRDP_TILE_CLAIM);
			if (begin >= limit)
				break;

			UINT32 end = begin + HMRDP_TILE_CLAIM;
			if (end > limit)
				end = limit;

			for (UINT32 index = begin; index < end; index++)
				progressive_process_tiles_tile_work_callback(instance, &chunk->params[index], work);
		}
	}
'@) 'this thread decodes only this chunk'

# (b3) allocate the arena once, and pin the serial branch to slot 0.
Patch-Regex $progScratchC `
  '\tif \(hmrdp_decode_width\(\) <= 1\)\n\t\{\n\t\t/\* Serial \(width 1, or no platform executor\): one call per tile, no task\n\t\t \* submission\. \*/\n' (@'
	/* HmRdp: the tile decode's working buffers come from this context's arena
	 * (one slot per chunk), allocated on the first message and never taken per
	 * tile - see hmrdp_tile_scratch(). */
	if (!progressive->tileScratch && !hmrdp_alloc_tile_scratch(progressive))
	{
		WLog_Print(progressive->log, WLOG_ERROR, "Failed to allocate the tile decode scratch");
		return -1;
	}

	if (hmrdp_region_chunks(region->numTiles) <= 1)
	{
		/* Serial: one call per tile, no task submission. Either there is no
		 * platform executor (or a single-core machine), or the region is too
		 * small to keep even two threads busy (hmrdp_region_chunks). */
		g_HmrdpTlsTileScratch = progressive->tileScratch;
'@) '!hmrdp_alloc_tile_scratch(progressive))'

# (b4) the shared claim cursor, claimed in blocks; one scratch slot per chunk.
Patch-Regex $progScratchC `
  '\t\{\n\t\tPROGRESSIVE_TILE_CHUNK_PARAM chunks\[HMRDP_TILE_CHUNKS\];\n\t\tvolatile UINT32 nextTile = 0;\n\t\tconst UINT32 numTiles = region->numTiles;\n\t\tconst UINT32 numChunks = numTiles < HMRDP_TILE_CHUNKS \? numTiles : HMRDP_TILE_CHUNKS;\n\n\t\tfor \(UINT32 c = 0; c < numChunks; c\+\+\)\n\t\t\{\n\t\t\tPROGRESSIVE_TILE_CHUNK_PARAM\* chunk = &chunks\[c\];\n\t\t\tchunk->params = progressive->params;\n\t\t\tchunk->next = &nextTile;\n\t\t\tchunk->numTiles = numTiles;\n\n\t\t\tprogressive->work_objects\[c\] =' (@'
	{
		PROGRESSIVE_TILE_CHUNK_PARAM chunks[HMRDP_TILE_CHUNKS];
		const UINT32 numTiles = region->numTiles;
		/* HmRdp: the chunk count = one scratch slot per chunk. This is the upper
		 * bound; the parallel path picks how many of these slots to submit. */
		UINT32 numChunks = HMRDP_TILE_CHUNKS;
		if (numChunks > numTiles)
			numChunks = numTiles;

		for (UINT32 c = 0; c < numChunks; c++)
		{
			PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
			chunk->params = progressive->params;
			chunk->scratch =
			    progressive->tileScratch + ((size_t)c * (size_t)HMRDP_TILE_SCRATCH_STRIDE);

			progressive->work_objects[c] =
'@) 'the chunk count = one scratch slot per chunk'


# (d) release the arena with the context.
Patch-Regex $progScratchC `
  '\tBufferPool_Free\(progressive->bufferPool\);\n\tHashTable_Free\(progressive->SurfaceContexts\);' (@'
	BufferPool_Free(progressive->bufferPool);
	winpr_aligned_free(progressive->tileScratch);
	HashTable_Free(progressive->SurfaceContexts);
'@) 'winpr_aligned_free(progressive->tileScratch)'
