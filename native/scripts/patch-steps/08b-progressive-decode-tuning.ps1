# 8) CPU (gdi) progressive-decode tuning: remove per-tile overhead without changing output -
#    chunked tile dispatch, region16-free clipping with per-pass stamps, masked 32bpp copy.
$progH = "$Source\libfreerdp\codec\progressive.h"
$progC = "$Source\libfreerdp\codec\progressive.c"
$copyC = "$Source\libfreerdp\primitives\prim_copy.c"

# (a1) per-tile / per-surface update stamps.
Patch-Regex $progH '\tBYTE flags;\n\tBYTE quality;\n\tBOOL dirty;\n' (@'
	BYTE flags;
	BYTE quality;
	BOOL dirty;
	/* HmRdp: last update_tiles() pass that composited this tile (visit each tile once per pass). */
	UINT32 updateStamp;
'@) 'updateStamp;'

Patch-Regex $progH '\tUINT32 numUpdatedTiles;\n\tUINT32\* updatedTileIndices;\n\} PROGRESSIVE_SURFACE_CONTEXT;' (@'
	UINT32 numUpdatedTiles;
	UINT32* updatedTileIndices;
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
} PROGRESSIVE_SURFACE_CONTEXT;
'@) 'monotonic stamp handed out by each update_tiles'

# (a2) chunked, dynamically balanced tile dispatch.
Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/*
 * HmRdp: the tile work is submitted in a small number of chunks.
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

		for (UINT32 c = 0; c < close_cnt; c++)
		{
			WaitForThreadpoolWorkCallbacks(progressive->work_objects[c], FALSE);
			CloseThreadpoolWork(progressive->work_objects[c]);
		}
	}

fail:
'@) 'the per-tile clipping used to go through region16'

# (a4) update_tiles: no per-tile REGION16, one visit per tile per pass.
Patch-Regex $progC '\tfor \(UINT32 i = 0; i < surface->numUpdatedTiles; i\+\+\)\n\t\{\n\t\tUINT32 nbUpdateRects = 0;.*?\n\t\tregion16_uninit\(&updateRegion\);\n\t\ttile->dirty = FALSE;\n\t\}\n' (@'
	/* HmRdp: the per-tile clipping used to go through region16 (two allocs + a free per tile);
	 * clippingRects is already non-overlapping, and the stamp visits each tile once per pass. */
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

			/* Bands are top-ordered, so anything past the tile cannot intersect. */
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
	}
'@) 'clippingList'

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

	/* HmRdp: keep-destination-alpha 32bpp copies are the hottest loop of the gdi path;
	 * one masked 32-bit move per pixel (dest 4th byte kept, low 3 from source) instead of three byte moves. */
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

