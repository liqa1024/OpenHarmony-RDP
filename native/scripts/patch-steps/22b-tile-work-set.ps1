# 22b) Tile work set comes from the clip rects ∩ tiles decoded this frame (hmrdpFrameId), with the
#      per-tile composite folded into a shared helper; same tiles/pixels as before.
$progWorkH = "$Source\libfreerdp\codec\progressive.h"
$progWorkC = "$Source\libfreerdp\codec\progressive.c"

$progWorkTileAnchor = "`tUINT32 updateStamp;`n`tUINT16 yLen;"
$progWorkTileNew = "`tUINT32 updateStamp;`n" +
"`t/* HmRdp: the frame this tile was last decoded in. update_tiles finds work from the clip rect ranges instead of the per-frame list. */`n" +
"`tUINT32 hmrdpFrameId;`n`tUINT16 yLen;"
Patch-Block $progWorkH $progWorkTileAnchor $progWorkTileNew 'hmrdpFrameId'

$progWorkAddAnchor = "`t`tsurface->updatedTileIndices[surface->numUpdatedTiles++] = (UINT32)zIdx;`n`t}"
$progWorkAddNew = "`t`tsurface->updatedTileIndices[surface->numUpdatedTiles++] = (UINT32)zIdx;`n" +
"`t`t/* HmRdp: remember the frame so update_tiles can find this tile from the clip rects. */`n" +
"`t`tt->hmrdpFrameId = surface->frameId;`n`t}"
Patch-Block $progWorkC $progWorkAddAnchor $progWorkAddNew 't->hmrdpFrameId = surface->frameId;'

$progWorkHelper = @'
/* HmRdp: composite one tile against one clip rect (destination coords); returns -1 hard failure, 1 copy failed, 0 ok. */
static INLINE int hmrdp_composite_tile(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive,
                                       PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface,
                                       RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile,
                                       const RECTANGLE_16* WINPR_RESTRICT clip,
                                       BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat,
                                       UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
                                       REGION16* WINPR_RESTRICT invalidRegion)
{
	RECTANGLE_16 updateRect = { 0 };
	RECTANGLE_16 common = { 0 };
	UINT32 nXSrc = 0;
	UINT32 nYSrc = 0;
	UINT32 width = 0;
	UINT32 height = 0;

	updateRect.left = (UINT16)(nXDst + tile->x);
	updateRect.top = (UINT16)(nYDst + tile->y);
	updateRect.right = (UINT16)(updateRect.left + 64);
	updateRect.bottom = (UINT16)(updateRect.top + 64);

	/* Bands are top-ordered, so anything past the tile cannot intersect. */
	if (clip->top >= updateRect.bottom)
		return 0;
	if (clip->bottom <= updateRect.top)
		return 0;
	if (!rectangles_intersection(clip, &updateRect, &common))
		return 0;

	if (common.left < updateRect.left)
		return -1;
	nXSrc = common.left - updateRect.left;
	nYSrc = common.top - updateRect.top;
	width = common.right - common.left;
	height = common.bottom - common.top;

	if (common.left + width > surface->width)
		return -1;
	if (common.top + height > surface->height)
		return -1;

	if (!freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, common.left, common.top, width,
	                                   height, tile->data, progressive->format, tile->stride, nXSrc,
	                                   nYSrc, NULL, FREERDP_KEEP_DST_ALPHA))
		return 1;

	/* HmRdp: O(1) damage bookkeeping instead of a region16 union (O(region) per tile); the span covers whole tiles. */
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

	return 0;
}
'@
$progWorkUpdate = @'
static INLINE BOOL update_tiles(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive,
                                PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface,
                                BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat, UINT32 nDstStep,
                                UINT32 nXDst, UINT32 nYDst,
                                PROGRESSIVE_BLOCK_REGION* WINPR_RESTRICT region,
                                REGION16* WINPR_RESTRICT invalidRegion)
{
	BOOL rc = TRUE;
	REGION16 clippingRects = { 0 };
	region16_init(&clippingRects);
	for (UINT32 i = 0; i < region->numRects; i++)
	{
		RECTANGLE_16 clippingRect = { 0 };
		const RFX_RECT* rect = &(region->rects[i]);

		clippingRect.left = (UINT16)nXDst + rect->x;
		clippingRect.top = (UINT16)nYDst + rect->y;
		clippingRect.right = clippingRect.left + rect->width;
		clippingRect.bottom = clippingRect.top + rect->height;
		region16_union_rect(&clippingRects, &clippingRects, &clippingRect);
	}

	/* HmRdp: clip via the region's already-non-overlapping rect list instead of a per-tile region16. */
	const UINT32 stamp = ++surface->updateStamp;
	UINT32 nbClipping = 0;
	const RECTANGLE_16* clippingList = region16_rects(&clippingRects, &nbClipping);

	/* HmRdp: walk only the tiles the clip rects can reach (tiles decoded this frame); same work set as the list walk. */
	for (UINT32 j = 0; j < nbClipping; j++)
	{
		const RECTANGLE_16* clip = &clippingList[j];
		const UINT32 clipLeft = (clip->left > nXDst) ? (UINT32)(clip->left - nXDst) : 0;
		const UINT32 clipTop = (clip->top > nYDst) ? (UINT32)(clip->top - nYDst) : 0;
		const UINT32 clipRight = (clip->right > nXDst) ? (UINT32)(clip->right - nXDst) : 0;
		const UINT32 clipBottom = (clip->bottom > nYDst) ? (UINT32)(clip->bottom - nYDst) : 0;
		const UINT32 lastX = surface->gridWidth > 0 ? surface->gridWidth - 1 : 0;
		const UINT32 lastY = surface->gridHeight > 0 ? surface->gridHeight - 1 : 0;
		UINT32 firstX = 0;
		UINT32 firstY = 0;
		UINT32 endX = 0;
		UINT32 endY = 0;
		UINT32 yIdx = 0;

		if ((clipRight == 0) || (clipBottom == 0))
			continue;

		firstX = clipLeft / 64;
		firstY = clipTop / 64;
		if (firstX > lastX || firstY > lastY)
			continue;

		endX = (clipRight - 1) / 64;
		endY = (clipBottom - 1) / 64;
		if (endX > lastX)
			endX = lastX;
		if (endY > lastY)
			endY = lastY;

		for (yIdx = firstY; yIdx <= endY; yIdx++)
		{
			UINT32 xIdx = 0;

			for (xIdx = firstX; xIdx <= endX; xIdx++)
			{
				RFX_PROGRESSIVE_TILE* tile = NULL;
				const UINT32 zIdx = (yIdx * surface->gridWidth) + xIdx;
				int cres = 0;

				if (zIdx >= surface->tilesSize)
					continue;
				tile = surface->tiles[zIdx];
				if (!tile)
					continue;
				if (tile->hmrdpFrameId != surface->frameId)
					continue;

				if (tile->updateStamp != stamp)
				{
					tile->updateStamp = stamp;
					tile->dirty = FALSE;
				}

				cres = hmrdp_composite_tile(progressive, surface, tile, clip, pDstData, DstFormat,
				                            nDstStep, nXDst, nYDst, invalidRegion);
				if (cres < 0)
				{
					/* Geometry guard: the original bails out without touching rc. */
					goto fail;
				}
				if (cres > 0)
				{
					/* Copy failed: stop this tile's clip loop, continue with the next tile. */
					rc = FALSE;
					break;
				}
			}
		}
	}
fail:
	region16_uninit(&clippingRects);
	return rc;
}
'@
Patch-Regex $progWorkC 'static INLINE BOOL update_tiles\(.*?\n\}\n' ($progWorkHelper + "`n`n" + $progWorkUpdate + "`n") 'hmrdp_composite_tile(progressive, surface, tile, clip'

