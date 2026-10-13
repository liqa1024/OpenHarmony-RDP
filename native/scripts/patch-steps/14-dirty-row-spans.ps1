# 14) Record the dirty area as one [left,right) span per tile row instead of a region16 union per
#     tile (the union is O(region) against a region that grows all frame); the composer folds them in once.
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
	/* HmRdp: dirty area as one [left,right) span of tile columns per tile row (see HmrdpProgressiveFlushDirty). */
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

	/* HmRdp: one dirty span per tile row (sized from the tile grid); allocation failure falls back to region16. */
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
			/* HmRdp: O(1) damage bookkeeping instead of a region16 union (O(region) per tile); the span covers whole tiles and the consumer clips. */
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

$flushFn = @'

/* HmRdp: fold the dirty tile-row spans into `out` (one rect per dirty row) and reset them. */
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
/* HmRdp: fold the progressive decoder's dirty tile-row spans into `out` (one union per dirty row). */
FREERDP_API BOOL HmrdpProgressiveFlushDirty(PROGRESSIVE_CONTEXT* progressive, UINT16 surfaceId,
                                            REGION16* out);


'@
# The anchor is tab-indented inside extern "C"; consume the tab and re-add it with the block.
$progFlushAnchor = "`tFREERDP_API INT32 progressive_decompress("
Patch-Block $progRowsApi $progFlushAnchor `
  ($progFlushDecl + $progFlushAnchor.TrimStart("`t")) 'HmrdpProgressiveFlushDirty'

$gdiFlushCall = @'
	/* HmRdp: fold the decoder's per-tile-row damage spans into the region (one union per dirty row). */
	if (surface->codecs != NULL && surface->codecs->progressive != NULL)
		HmrdpProgressiveFlushDirty(surface->codecs->progressive, surface->surfaceId,
		                           &(surface->invalidRegion));


'@
Patch-Block $gdiRowsC "`tsurfaceX = surface->outputOriginX;" `
  ($gdiFlushCall + "`tsurfaceX = surface->outputOriginX;") `
  'HmrdpProgressiveFlushDirty(surface->codecs->progressive'

