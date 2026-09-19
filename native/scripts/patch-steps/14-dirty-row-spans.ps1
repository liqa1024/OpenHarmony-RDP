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

$progStampAnchor = @'
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
'@
Patch-Block $progRowsH $progStampAnchor (@'
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


'@
# The declaration below is indented with a tab inside `extern "C" {`; consume it
# (the inserted comment starts at column 0, like the tree) and re-add the
# declaration after the new block.
$progFlushAnchor = "`tFREERDP_API INT32 progressive_decompress("
Patch-Block $progRowsApi $progFlushAnchor `
  ($progFlushDecl + $progFlushAnchor.TrimStart("`t")) 'HmrdpProgressiveFlushDirty'

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

