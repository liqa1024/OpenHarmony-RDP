# 22b) HmRdp: the tile work set comes from the clip rects, and the per-tile
#      composite is one helper both paths share.
#
#      背景（doc_agent/cpu-accel-plan.md §2）：update_tiles 原先走整帧累积的
#      updatedTileIndices（一条消息一次解码 ⇒ 同一 tile 一帧内出现多次，整条列表
#      每条消息都要重走），并且内联做 region16 求交 + 像素拷贝。这一步把工作集改成
#      "裁剪矩形覆盖到的 tile 范围 ∩ 本帧解码过的 tile"（hmrdpFrameId），把逐 tile
#      的裁剪求交 + 拷贝 + 脏区 span 记账收进 hmrdp_composite_tile，供 update_tiles
#      与 tile 解码侧的直写共用（step 23 的 hmrdp_tile_copy_now 与它同一套几何）。
#
#      结果不变：同一批 tile、同样的裁剪、同样的像素与脏区，只是遍历范围与调用
#      组织不同。整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progWorkH = "$Source\libfreerdp\codec\progressive.h"
$progWorkC = "$Source\libfreerdp\codec\progressive.c"

# (a) the tile remembers the frame it was last decoded in.
$progWorkTileAnchor = "`tUINT32 updateStamp;`n`tUINT16 yLen;"
$progWorkTileNew = "`tUINT32 updateStamp;`n" +
"`t/* HmRdp: the frame this tile was last decoded in. update_tiles' work set is`n" +
"`t * ""the tiles decoded in the current frame, clipped to this message's rects"";`n" +
"`t * the upstream per-frame list is one entry per decode and gets re-walked in`n" +
"`t * full for every message of the frame, which costs far more than the copies`n" +
"`t * (see the patch note in native/scripts/patch-freerdp.ps1 step 20). A clip`n" +
"`t * rect names the tile range it can reach, and this id tells the tiles in that`n" +
"`t * range that have pixels for the frame. */`n" +
"`tUINT32 hmrdpFrameId;`n`tUINT16 yLen;"
Patch-Block $progWorkH $progWorkTileAnchor $progWorkTileNew 'hmrdpFrameId'

# (b) remember it when the tile joins the frame's update list.
$progWorkAddAnchor = "`t`tsurface->updatedTileIndices[surface->numUpdatedTiles++] = (UINT32)zIdx;`n`t}"
$progWorkAddNew = "`t`tsurface->updatedTileIndices[surface->numUpdatedTiles++] = (UINT32)zIdx;`n" +
"`t`t/* HmRdp: remember the frame, so update_tiles can find this tile from the`n" +
"`t`t * clip rects instead of walking the list (step 20). */`n" +
"`t`tt->hmrdpFrameId = surface->frameId;`n`t}"
Patch-Block $progWorkC $progWorkAddAnchor $progWorkAddNew 't->hmrdpFrameId = surface->frameId;'

# (c) insert the shared composite helper and rewrite update_tiles to walk the
#     clipping rects' tile ranges instead of the whole per-frame list.
$progWorkHelper = @'
/*
 * HmRdp: composite one tile against one clipping rect (both in destination
 * coordinates). Returns -1 for the geometry guards the original treats as a hard
 * failure, 1 when the copy itself failed (the original breaks that tile's clip
 * loop and continues with the next tile), 0 otherwise.
 */
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

	/* region16_rects() returns the region's bands ordered by top, so everything
	 * past the tile cannot intersect it. */
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

	/* HmRdp: O(1) damage bookkeeping instead of a region16 union (which is
	 * O(region), once per tile, against a region that grows all frame). The span
	 * covers whole tiles, i.e. a superset of what was copied - the extra pixels
	 * are the surface's own content, and the consumer clips the result to the
	 * surface. */
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

	/*
	 * HmRdp: the per-tile clipping used to go through region16
	 * (region16_init/intersect_rect/rects/uninit), i.e. two allocations and a
	 * free per visited tile, on the order of a thousand times per full-screen
	 * frame. The result is the same set of pixels - clippingRects is a canonical
	 * region, so its rect list is already non-overlapping - and reading that list
	 * once and intersecting with plain arithmetic (with the same ordered early
	 * exit region16_intersect_rect() takes) removes the allocator from the hot
	 * path without changing which pixels are written.
	 *
	 * The stamp keeps each tile visited at most once per pass: the per-frame
	 * list gets one entry per *decode*, so a tile refined by a later message of
	 * the same frame appears several times, and every occurrence used to copy the
	 * same (already final) tile data to the same place.
	 */
	const UINT32 stamp = ++surface->updateStamp;
	UINT32 nbClipping = 0;
	const RECTANGLE_16* clippingList = region16_rects(&clippingRects, &nbClipping);

	/*
	 * HmRdp: walk the tiles the clipping rects can reach, not the frame's whole
	 * tile list (see the patch note in native/scripts/patch-freerdp.ps1 step 20).
	 * The work set is the same - tiles decoded in this frame, clipped to this
	 * message's rects - and so is the result; the list walk below stays as the
	 * fallback for a tile that has no frame id yet.
	 */
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
					/* The geometry guards the original bails out on, without
					 * touching rc. */
					goto fail;
				}
				if (cres > 0)
				{
					/* The copy failed: the original stops this tile's clip loop
					 * and carries on with the next tile. */
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

