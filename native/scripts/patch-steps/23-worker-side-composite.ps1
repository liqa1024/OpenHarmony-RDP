# 23) Tile decode composites the tile itself (pixels hot, spread over workers); update_tiles keeps
#     only the damage bookkeeping, skipping its copy when the clip hash matches. Applied as one block.
$progWcC = "$Source\libfreerdp\codec\progressive.c"
$progWcH = "$Source\libfreerdp\codec\progressive.h"

Patch-Regex $progWcH `
  '\tUINT32 updateStamp;\n\t/\* HmRdp: the frame this tile was last decoded in\.' (@'
	UINT32 updateStamp;
	/* HmRdp: set when the tile decode already composited this tile (clip hash stored below). */
	BOOL hmrdpCopied;
	UINT32 hmrdpCopyClip;
	/* HmRdp: the frame this tile was last decoded in.
'@) 'hmrdpCopyClip'

Patch-Regex $progWcH `
  '\t/\* HmRdp: monotonic stamp handed out by each update_tiles\(\) pass\. \*/\n\tUINT32 updateStamp;' (@'
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
	/* HmRdp: hash of the clip the current update_tiles() pass composites with. */
	UINT32 hmrdpUpdateClip;
'@) 'hmrdpUpdateClip'

Patch-Regex $progWcH `
  '\tBYTE\* tileScratch;\n\};' (@'
	BYTE* tileScratch;
	/* HmRdp: destination + merged clip of the region being decoded, stashed so the tile decode can composite itself. */
	BYTE* hmrdpDstData;
	UINT32 hmrdpDstFormat;
	UINT32 hmrdpDstStep;
	UINT32 hmrdpDstX;
	UINT32 hmrdpDstY;
	BOOL hmrdpDstValid;
	const RECTANGLE_16* hmrdpClip;
	UINT32 hmrdpClipCount;
	UINT32 hmrdpClipHash;
	/* The surface of the current message (the tile work params do not carry it). */
	PROGRESSIVE_SURFACE_CONTEXT* hmrdpSurface;
	/* Bumped once per Progressive message, so a stale "already copied" flag cannot match the next clip. */
	UINT32 hmrdpMsgSeq;
};
'@) 'hmrdpMsgSeq'

Patch-Regex $progWcC `
  'static void CALLBACK progressive_process_tiles_tile_work_callback\(' (@'
/* HmRdp: composite each tile right after decode (pixels hot, work spreads over workers); update_tiles keeps only damage bookkeeping. */
#define HMRDP_WORKER_TILE_COPY 1

/* HmRdp: clip identity; the message sequence is folded in so a stale per-tile flag cannot match the next message's clip. */
static INLINE UINT32 hmrdp_clip_hash(UINT32 msgSeq, const RECTANGLE_16* rects, UINT32 count)
{
	UINT32 h = 2166136261u ^ (msgSeq * 2654435761u);

	for (UINT32 i = 0; i < count; i++)
	{
		h = (h ^ rects[i].left) * 16777619u;
		h = (h ^ rects[i].top) * 16777619u;
		h = (h ^ rects[i].right) * 16777619u;
		h = (h ^ rects[i].bottom) * 16777619u;
	}
	return h;
}

/* HmRdp: composite `tile` into the stashed destination with the stashed clip (same geometry as hmrdp_composite_tile). */
static INLINE unsigned long long
hmrdp_tile_copy_now(PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive,
                    PROGRESSIVE_SURFACE_CONTEXT* WINPR_RESTRICT surface,
                    RFX_PROGRESSIVE_TILE* WINPR_RESTRICT tile)
{
	RECTANGLE_16 updateRect = { 0 };
	unsigned long long written = 0;

	if ((progressive == NULL) || (surface == NULL) || (tile == NULL) || (!progressive->hmrdpDstValid) ||
	    (progressive->hmrdpClip == NULL))
		return 0;

	updateRect.left = (UINT16)(progressive->hmrdpDstX + tile->x);
	updateRect.top = (UINT16)(progressive->hmrdpDstY + tile->y);
	updateRect.right = (UINT16)(updateRect.left + 64);
	updateRect.bottom = (UINT16)(updateRect.top + 64);

	for (UINT32 j = 0; j < progressive->hmrdpClipCount; j++)
	{
		const RECTANGLE_16* clip = &progressive->hmrdpClip[j];
		RECTANGLE_16 common = { 0 };
		UINT32 nXSrc = 0;
		UINT32 nYSrc = 0;
		UINT32 width = 0;
		UINT32 height = 0;

		/* The list is ordered by top, so nothing past the tile can intersect. */
		if (clip->top >= updateRect.bottom)
			break;
		if (clip->bottom <= updateRect.top)
			continue;
		if (!rectangles_intersection(clip, &updateRect, &common))
			continue;
		if (common.left < updateRect.left)
			return written;

		nXSrc = common.left - updateRect.left;
		nYSrc = common.top - updateRect.top;
		width = common.right - common.left;
		height = common.bottom - common.top;

		if (common.left + width > surface->width)
			return written;
		if (common.top + height > surface->height)
			return written;

		if (!freerdp_image_copy_no_overlap(progressive->hmrdpDstData, progressive->hmrdpDstFormat,
		                                   progressive->hmrdpDstStep, common.left, common.top, width,
		                                   height, tile->data, progressive->format, tile->stride,
		                                   nXSrc, nYSrc, NULL, FREERDP_KEEP_DST_ALPHA))
			return written;

		written += (unsigned long long)width * (unsigned long long)height;
	}

	if (written > 0)
	{
		tile->hmrdpCopied = TRUE;
		tile->hmrdpCopyClip = progressive->hmrdpClipHash;
	}
	return written;
}

static void CALLBACK progressive_process_tiles_tile_work_callback(
'@) 'hmrdp_tile_copy_now'

Patch-Regex $progWcC `
  '\t\t\tbreak;\n\t\}\n\}\n\n/\*\n \* HmRdp: the tile work is submitted in a small number of chunks' (@'
			break;
	}

#if HMRDP_WORKER_TILE_COPY
	/* HmRdp: composite the tile right here; update_tiles only keeps the damage bookkeeping. */
	if (param->progressive->hmrdpDstValid)
	{
		(void)hmrdp_tile_copy_now(param->progressive, param->progressive->hmrdpSurface,
		                          param->tile);
	}
#endif
}

/*
 * HmRdp: the tile work is submitted in a small number of chunks
'@) 'param->progressive->hmrdpSurface'

Patch-Regex $progWcC `
  '\tPROGRESSIVE_BLOCK_REGION\* WINPR_RESTRICT region = &progressive->region;\n\tWINPR_ASSERT\(region\);' (@'
	PROGRESSIVE_BLOCK_REGION* WINPR_RESTRICT region = &progressive->region;
	WINPR_ASSERT(region);

	/* HmRdp: hand the tile decode the destination to composite into (it runs before update_tiles). */
	progressive->hmrdpDstData = pDstData;
	progressive->hmrdpDstFormat = DstFormat;
	progressive->hmrdpDstStep = nDstStep;
	progressive->hmrdpDstX = nXDst;
	progressive->hmrdpDstY = nYDst;
	progressive->hmrdpSurface = surface;
	progressive->hmrdpDstValid = TRUE;
	progressive->hmrdpMsgSeq++;
'@) 'progressive->hmrdpMsgSeq++'

Patch-Regex $progWcC `
  '\tif \(hmrdp_region_chunks\(region->numTiles\) <= 1\)\n' (@'
	/* HmRdp: the merged clipping rects update_tiles will use, built the same way so the tile decode can composite with the same clip. */
	REGION16 hmrdpClipRegion = { 0 };
	if (progressive->hmrdpDstValid)
	{
		region16_init(&hmrdpClipRegion);
		for (UINT32 i = 0; i < region->numRects; i++)
		{
			RECTANGLE_16 clippingRect = { 0 };
			const RFX_RECT* rect = &(region->rects[i]);

			clippingRect.left = (UINT16)progressive->hmrdpDstX + rect->x;
			clippingRect.top = (UINT16)progressive->hmrdpDstY + rect->y;
			clippingRect.right = clippingRect.left + rect->width;
			clippingRect.bottom = clippingRect.top + rect->height;
			region16_union_rect(&hmrdpClipRegion, &hmrdpClipRegion, &clippingRect);
		}
		progressive->hmrdpClip = region16_rects(&hmrdpClipRegion, &progressive->hmrdpClipCount);
		progressive->hmrdpClipHash = hmrdp_clip_hash(progressive->hmrdpMsgSeq,
		                                             progressive->hmrdpClip,
		                                             progressive->hmrdpClipCount);
	}

	if (hmrdp_region_chunks(region->numTiles) <= 1)
'@ + "`n") 'hmrdpClipRegion'

# ... and takes it back at the end of the region.
Patch-Regex $progWcC `
  'fail:\n\n\tif \(status < 0\)\n\t\treturn -1;\n\n\treturn \(SSIZE_T\)\(end - start\);' (@'
fail:

	/* HmRdp: the stashed clip dies with the region decode. */
	progressive->hmrdpClip = NULL;
	progressive->hmrdpClipCount = 0;
	region16_uninit(&hmrdpClipRegion);

	if (status < 0)
		return -1;

	return (SSIZE_T)(end - start);
'@) 'the stashed clip dies with the region decode'

Patch-Regex $progWcC `
  '\tconst RECTANGLE_16\* clippingList = region16_rects\(&clippingRects, &nbClipping\);' (@'
	const RECTANGLE_16* clippingList = region16_rects(&clippingRects, &nbClipping);

	/* HmRdp: identify this pass's clip so a tile already copied with it can skip its copy. */
	surface->hmrdpUpdateClip =
	    hmrdp_clip_hash(progressive->hmrdpMsgSeq, clippingList, nbClipping);
'@) 'surface->hmrdpUpdateClip'

Patch-Regex $progWcC `
  '\tif \(!freerdp_image_copy_no_overlap\(pDstData, DstFormat, nDstStep, common\.left, common\.top, width,\n\t                                   height, tile->data, progressive->format, tile->stride, nXSrc,\n\t                                   nYSrc, NULL, FREERDP_KEEP_DST_ALPHA\)\)\n\t\treturn 1;\n' (@'
	if (tile->hmrdpCopied && (tile->hmrdpCopyClip == surface->hmrdpUpdateClip))
	{
		/* HmRdp: the tile decode already wrote exactly these pixels with this clip - only the damage bookkeeping below is left. */
	}
	else if (!freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, common.left, common.top,
	                                        width, height, tile->data, progressive->format,
	                                        tile->stride, nXSrc, nYSrc, NULL,
	                                        FREERDP_KEEP_DST_ALPHA))
		return 1;

'@) 'already wrote exactly these pixels'

# Self-check: a here-string not terminated on its own line would leak this file into the C source.
foreach ($f in @($progWcC, $progWcH))
{
	$text = [System.IO.File]::ReadAllText($f)
	if ($text.Contains("'@)"))
	{
		throw "HmRdp step 23: leaked patch text in $f - a here-string terminator is not on its own line"
	}
}
