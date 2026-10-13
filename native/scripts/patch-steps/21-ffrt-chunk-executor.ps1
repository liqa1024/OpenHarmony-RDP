# 21) Run the tile-decode chunks on the platform task queue (ffrt) instead of the WinPR pool (the
#     pool scaled poorly); weak symbols fall back to the serial branch. Applied as one block.
$progParC = "$Source\libfreerdp\codec\progressive.c"

Patch-Regex $progParC `
  '/\* HmRdp: the decode width requested by the app \(hmrdp_parallel\.\*\)\. Weak, so a\n \* build without the platform executor decodes on the receiving thread\. \*/\nextern unsigned int HmrdpDecodeWidth\(void\) __attribute__\(\(weak\)\);\n\nstatic INLINE UINT32 hmrdp_decode_width\(void\)\n\{\n\treturn HmrdpDecodeWidth != NULL \? HmrdpDecodeWidth\(\) : 1u;\n\}' (@'
/* HmRdp: decode width (hmrdp_parallel.*); weak, so a build without the executor decodes serially. */
extern unsigned int HmrdpDecodeWidth(void) __attribute__((weak));

static INLINE UINT32 hmrdp_decode_width(void)
{
	return HmrdpDecodeWidth != NULL ? HmrdpDecodeWidth() : 1u;
}

/* HmRdp: min tiles per chunk to be worth its own thread (the tuning point for the submit/wake trade-off). */
#define HMRDP_MIN_TILES_PER_WORKER 64

/* HmRdp: how many chunks this region is worth (0 = do not split; caller takes the serial branch). */
static INLINE UINT32 hmrdp_region_chunks(UINT32 numTiles)
{
	UINT32 chunks = numTiles / HMRDP_MIN_TILES_PER_WORKER;
	const UINT32 width = hmrdp_decode_width();
	if (chunks > width)
		chunks = width;
	if (chunks > HMRDP_TILE_CHUNKS)
		chunks = HMRDP_TILE_CHUNKS;
	return chunks;
}

/* HmRdp: the platform task queue (ffrt), exported by the app (hmrdp_parallel.*); weak, so a
 * build without it forces the serial branch. */
extern int HmrdpParallelAvailable(void) __attribute__((weak));
extern int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx)
    __attribute__((weak));
'@) 'the platform task queue (ffrt)'

Patch-Regex $progParC `
  '/\* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above\. \*/\n' (@'
/* HmRdp: one ffrt task per chunk; forwards to the same chunk callback the tile work uses. */
static void hmrdp_chunk_ffrt_callback(void* ctx, unsigned int index)
{
	PROGRESSIVE_TILE_CHUNK_PARAM* chunks = (PROGRESSIVE_TILE_CHUNK_PARAM*)ctx;

	progressive_process_tile_chunk_callback((PTP_CALLBACK_INSTANCE)0, (void*)&chunks[index],
	                                        (PTP_WORK)0);
}

/* HMRDP_TILE_CHUNKS is defined with the tile scratch helpers above. */
'@) 'hmrdp_chunk_ffrt_callback'

Patch-Regex $progParC `
  '\t\tUINT32 numChunks = HMRDP_TILE_CHUNKS;.*?\n\t\t\tCloseThreadpoolWork\(progressive->work_objects\[c\]\);\n\t\t\}\n' (@'
		/* HmRdp: one task per home range (each task owns a contiguous range, then steals
		 * others' HMRDP_TILE_CLAIM blocks); the receiving thread runs one, so n homes use n threads. */
		const UINT32 numChunks = hmrdp_region_chunks(numTiles);

		_Alignas(64) volatile UINT32 homeNextArr[HMRDP_TILE_CHUNKS];
		UINT32 homeLimitsArr[HMRDP_TILE_CHUNKS];
		const UINT32 homeCount = numChunks;
		for (UINT32 h = 0; h < homeCount; h++)
		{
			homeNextArr[h] = (UINT32)(((unsigned long long)h * numTiles) / homeCount);
			homeLimitsArr[h] = (UINT32)(((unsigned long long)(h + 1) * numTiles) / homeCount);
		}

		if (HmrdpParallelAvailable != NULL && HmrdpParallelRun != NULL &&
		    (HmrdpParallelAvailable() != 0))
		{
			/* HmRdp: one ffrt task per home; a region is only a few ms of work, so more task objects cost more than they buy. */
			for (UINT32 c = 0; c < numChunks; c++)
			{
				PROGRESSIVE_TILE_CHUNK_PARAM* chunk = &chunks[c];
				chunk->params = progressive->params;
				chunk->scratch =
				    progressive->tileScratch + ((size_t)c * (size_t)HMRDP_TILE_SCRATCH_STRIDE);
				chunk->homeNext = homeNextArr;
				chunk->homeLimits = homeLimitsArr;
				chunk->homeCount = homeCount;
				chunk->homeIndex = c;
			}

			(void)HmrdpParallelRun(numChunks, hmrdp_chunk_ffrt_callback, (void*)chunks);
			goto fail;
		}

		/* No platform queue: decode on the receiving thread (belt and braces; width already forces serial). */
		g_HmrdpTlsTileScratch = progressive->tileScratch;
		for (UINT32 idx = 0; idx < numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);
'@ + "`n") 'one task per home range'
