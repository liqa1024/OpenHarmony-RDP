# 20) Tile decode: give each chunk its own scratch slot (no per-tile pool Take/Return) and claim
#     tiles in blocks (cut shared-counter traffic); pixels unchanged. Applied as one block.
$progScratchH = "$Source\libfreerdp\codec\progressive.h"
$progScratchC = "$Source\libfreerdp\codec\progressive.c"

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

Patch-Regex $progScratchC `
  '#define TAG FREERDP_TAG\("codec\.progressive"\)\n' (@'
#define TAG FREERDP_TAG("codec.progressive")

/* HmRdp: per-chunk tile working buffers (coefficient buffer + inverse-DWT scratch); each
 * dispatched chunk owns one two-buffer slot allocated once per context, so no per-tile lock. */
#define HMRDP_TILE_CHUNKS 64
#define HMRDP_TILE_CLAIM 2
#define HMRDP_TILE_SCRATCH_BYTES ((8192ULL + 32ULL) * 3ULL)
#define HMRDP_TILE_SCRATCH_STRIDE (HMRDP_TILE_SCRATCH_BYTES * 2ULL)

static _Thread_local BYTE* g_HmrdpTlsTileScratch = NULL;

/* The current thread's chunk slot, or slot 0 outside the chunk dispatch (serial branch). */
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

Patch-Regex-All $progScratchC `
  '/\* Upper bound on the work items a single region may be split into\. \*/\n#define HMRDP_TILE_CHUNKS 64' `
  '/* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above. */' `
  'is defined with the tile scratch helpers'

Patch-Regex $progScratchC `
  '\tINT16\* temp = \(INT16\*\)BufferPool_Take\(progressive->bufferPool, -1\); /\* DWT buffer \*/\n\n\tif \(!temp\)\n\t\treturn -2;\n' (@'
	/* HmRdp: the inverse-DWT scratch is the current chunk's second slot (see hmrdp_tile_scratch). */
	INT16* temp = hmrdp_tile_dwt_scratch(progressive);

'@) 'hmrdp_tile_dwt_scratch(progressive)'

Patch-Regex $progScratchC '\tBufferPool_Return\(progressive->bufferPool, temp\);\n\treturn 1;' (@'
	/* HmRdp: no pool Return for the DWT scratch - the slot is reused as is. */
	return 1;
'@) 'no pool Return for the DWT scratch'

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

Patch-Regex $progScratchC `
  'typedef struct\n\{\n\tPROGRESSIVE_TILE_PROCESS_WORK_PARAM\* params;\n\tvolatile UINT32\* next;\n\tUINT32 numTiles;\n\} PROGRESSIVE_TILE_CHUNK_PARAM;' (@'
typedef struct
{
	PROGRESSIVE_TILE_PROCESS_WORK_PARAM* params;
	/* HmRdp: the chunk's own working-buffer slot (see hmrdp_tile_scratch). */
	BYTE* scratch;
	/* HmRdp: each chunk owns a contiguous home range, then steals other homes' remaining HMRDP_TILE_CLAIM blocks as one block. */
	volatile UINT32* homeNext;
	const UINT32* homeLimits;
	UINT32 homeCount;
	UINT32 homeIndex;
} PROGRESSIVE_TILE_CHUNK_PARAM;
'@) "the chunk's own working-buffer slot"

Patch-Regex $progScratchC `
  '\tfor \(;;\)\n\t\{\n\t\tconst UINT32 index = __sync_fetch_and_add\(chunk->next, 1u\);\n\t\tif \(index >= chunk->numTiles\)\n\t\t\tbreak;\n\n\t\tprogressive_process_tiles_tile_work_callback\(instance, &chunk->params\[index\], work\);\n\t\}' (@'
	/* HmRdp: this thread decodes only this chunk, so its working buffers are the chunk's slot. */
	g_HmrdpTlsTileScratch = chunk->scratch;

	/* HmRdp: home range first, then steal other homes' remaining blocks; claimed in HMRDP_TILE_CLAIM blocks. */
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

Patch-Regex $progScratchC `
  '\tif \(hmrdp_decode_width\(\) <= 1\)\n\t\{\n\t\t/\* Serial \(width 1, or no platform executor\): one call per tile, no task\n\t\t \* submission\. \*/\n' (@'
	/* HmRdp: tile working buffers come from this context's arena (one slot per chunk), allocated once. */
	if (!progressive->tileScratch && !hmrdp_alloc_tile_scratch(progressive))
	{
		WLog_Print(progressive->log, WLOG_ERROR, "Failed to allocate the tile decode scratch");
		return -1;
	}

	if (hmrdp_region_chunks(region->numTiles) <= 1)
	{
		/* Serial (no platform executor, or too small a region): one call per tile, no task submission. */
		g_HmrdpTlsTileScratch = progressive->tileScratch;
'@) '!hmrdp_alloc_tile_scratch(progressive))'

Patch-Regex $progScratchC `
  '\t\{\n\t\tPROGRESSIVE_TILE_CHUNK_PARAM chunks\[HMRDP_TILE_CHUNKS\];\n\t\tvolatile UINT32 nextTile = 0;\n\t\tconst UINT32 numTiles = region->numTiles;\n\t\tconst UINT32 numChunks = numTiles < HMRDP_TILE_CHUNKS \? numTiles : HMRDP_TILE_CHUNKS;\n\n\t\tfor \(UINT32 c = 0; c < numChunks; c\+\+\)\n\t\t\{\n\t\t\tPROGRESSIVE_TILE_CHUNK_PARAM\* chunk = &chunks\[c\];\n\t\t\tchunk->params = progressive->params;\n\t\t\tchunk->next = &nextTile;\n\t\t\tchunk->numTiles = numTiles;\n\n\t\t\tprogressive->work_objects\[c\] =' (@'
	{
		PROGRESSIVE_TILE_CHUNK_PARAM chunks[HMRDP_TILE_CHUNKS];
		const UINT32 numTiles = region->numTiles;
		/* HmRdp: the chunk count = one scratch slot per chunk; the parallel path picks how many to submit. */
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


Patch-Regex $progScratchC `
  '\tBufferPool_Free\(progressive->bufferPool\);\n\tHashTable_Free\(progressive->SurfaceContexts\);' (@'
	BufferPool_Free(progressive->bufferPool);
	winpr_aligned_free(progressive->tileScratch);
	HashTable_Free(progressive->SurfaceContexts);
'@) 'winpr_aligned_free(progressive->tileScratch)'
