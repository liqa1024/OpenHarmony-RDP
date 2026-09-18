# 23) HmRdp: the tile decode composites the tile itself; update_tiles only keeps
#     the damage bookkeeping.
#
#     动机（doc_agent/cpu-accel-plan.md §2）：并行段之外最大的一块是 `update`——把每个 tile
#     从 `tile->data` 拷进目标 surface（每 tile 48KB：读源 16KB + 读目的 16KB + 写目的 16KB），
#     整帧约 9ms 且完全落在接收线程上，占该帧 CPU 部分的约 4 成。
#
#     做法：目标缓冲与合并后的 clip 在 `progressive_decompress` 里存进 context，`tile` 解码完一块就
#     用同一套几何把这块直接写进 surface（像素正热、且这份搬运天然分摊到各 worker）；`update_tiles`
#     保留原有的逐 tile 遍历、O(1) 脏区 span 记账与计数器，**只跳过那次像素拷贝**。
#
#     正确性的关键：`update_tiles` 用的是**消息里最后一条 region** 的合并 clip，而 tile 解码是按
#     region 跑的 ⇒ 一条消息含多条 region 时两者可能不同。所以每个被直写的 tile 记下所用 clip 的
#     **哈希（折入消息序号，跨消息不可能重复）**，`update_tiles` 只在哈希一致时跳过拷贝，否则照旧
#     拷贝（后者写入在后，结果与参考一致）。判据仍是两份录像的 `bad=0`。
#
#     开关：`HMRDP_WORKER_TILE_COPY`（1 = 直写，0 = 全部留在 update_tiles）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progWcC = "$Source\libfreerdp\codec\progressive.c"
$progWcH = "$Source\libfreerdp\codec\progressive.h"

# (a) the tile remembers what it already composited.
Patch-Regex $progWcH `
  '\tUINT32 updateStamp;\n\t/\* HmRdp: the frame this tile was last decoded in\.' (@'
	UINT32 updateStamp;
	/* HmRdp: set when the tile decode already composited this tile into the
	 * destination (with the clip whose hash is stored here), so update_tiles can
	 * skip the pixel copy and keep only the damage bookkeeping - see the patch
	 * note in native/scripts/patch-freerdp.ps1 step 23. */
	BOOL hmrdpCopied;
	UINT32 hmrdpCopyClip;
	/* HmRdp: the frame this tile was last decoded in.
'@) 'hmrdpCopyClip'

# (b) the surface remembers the clip of the current update pass.
Patch-Regex $progWcH `
  '\t/\* HmRdp: monotonic stamp handed out by each update_tiles\(\) pass\. \*/\n\tUINT32 updateStamp;' (@'
	/* HmRdp: monotonic stamp handed out by each update_tiles() pass. */
	UINT32 updateStamp;
	/* HmRdp: hash of the clip the current update_tiles() pass composites with; a
	 * tile whose hmrdpCopyClip matches it was already copied with the same clip
	 * (see patch-freerdp.ps1 step 23). */
	UINT32 hmrdpUpdateClip;
'@) 'hmrdpUpdateClip'

# (c) the context carries the message's destination, clip and identity.
Patch-Regex $progWcH `
  '\tBYTE\* tileScratch;\n\};' (@'
	BYTE* tileScratch;
	/* HmRdp: the destination and the merged clipping rects of the region being
	 * decoded, stashed by progressive_decompress / progressive_process_tiles so the
	 * tile decode can composite a tile itself (the tile walk runs before
	 * update_tiles and has no other path to either). Valid for one call; the hash
	 * identifies the clip so update_tiles can tell whether a tile it sees was
	 * already copied with the same one - see patch-freerdp.ps1 step 23. */
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
	/* Bumped once per Progressive message, so a per-tile "already copied" flag can
	 * never be mistaken for the next message's clip. */
	UINT32 hmrdpMsgSeq;
};
'@) 'hmrdpMsgSeq'

# (d) the clip identity and the worker-side composite, next to the tile callback.
Patch-Regex $progWcC `
  'static void CALLBACK progressive_process_tiles_tile_work_callback\(' (@'
/* HmRdp: let the tile decode composite each tile itself, right after it is
 * decoded (the pixels are hot and the work spreads over the workers), and leave
 * update_tiles only its O(1) damage bookkeeping. See the patch note in
 * native/scripts/patch-freerdp.ps1 step 23. */
#define HMRDP_WORKER_TILE_COPY 1

/* HmRdp: the clip identity. A tile the decode composited stores this value, and
 * update_tiles skips its own copy only for a matching clip. The message sequence
 * is folded in so the value can never repeat across messages (a stale per-tile
 * flag must not silence a copy that is still needed). */
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

/* HmRdp: composite `tile` into the stashed destination with the stashed clip,
 * exactly like hmrdp_composite_tile does (same geometry guards, same primitive).
 * Returns the pixels written; also records the clip on the tile so
 * update_tiles can skip its copy. */
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

# (e) the tile callback composites right after the decode.
Patch-Regex $progWcC `
  '\t\t\tbreak;\n\t\}\n\}\n\n/\*\n \* HmRdp: the tile work is submitted in a small number of chunks' (@'
			break;
	}

#if HMRDP_WORKER_TILE_COPY
	/* HmRdp: composite the tile right here - the pixels are hot and the copy
	 * spreads over the workers; update_tiles only keeps the damage bookkeeping
	 * (see the patch note in native/scripts/patch-freerdp.ps1 step 23). */
	if (param->progressive->hmrdpDstValid)
	{
		const unsigned long long px = hmrdp_tile_copy_now(param->progressive,
		                                                  param->progressive->hmrdpSurface,
		                                                  param->tile);
		if (px > 0)
			__atomic_add_fetch(&HmrdpProgStat[19], px, __ATOMIC_RELAXED);
	}
#endif
}

/*
 * HmRdp: the tile work is submitted in a small number of chunks
'@) 'param->progressive->hmrdpSurface'

# (f) progressive_decompress hands the destination over.
Patch-Regex $progWcC `
  '\tPROGRESSIVE_BLOCK_REGION\* WINPR_RESTRICT region = &progressive->region;\n\tWINPR_ASSERT\(region\);' (@'
	PROGRESSIVE_BLOCK_REGION* WINPR_RESTRICT region = &progressive->region;
	WINPR_ASSERT(region);

	/* HmRdp: hand the tile decode the destination it should composite into (it
	 * runs before update_tiles and has no other path to it). The sequence number
	 * makes each message's clip hash unique. */
	progressive->hmrdpDstData = pDstData;
	progressive->hmrdpDstFormat = DstFormat;
	progressive->hmrdpDstStep = nDstStep;
	progressive->hmrdpDstX = nXDst;
	progressive->hmrdpDstY = nYDst;
	progressive->hmrdpSurface = surface;
	progressive->hmrdpDstValid = TRUE;
	progressive->hmrdpMsgSeq++;
'@) 'progressive->hmrdpMsgSeq++'

# (g) the region decode stashes the merged clip update_tiles will use...
Patch-Regex $progWcC `
  '\tif \(hmrdp_decode_width\(\) <= 1\)\n' (@'
	/* HmRdp: the merged clipping rects update_tiles will composite with, built the
	 * same way (region16 union, so the list is canonical and ordered by top), so
	 * the tile decode can composite with the same clip and stamp its hash on each
	 * tile (see the patch note in native/scripts/patch-freerdp.ps1 step 23). */
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

	if (hmrdp_decode_width() <= 1)
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

# (h) update_tiles identifies its clip.
Patch-Regex $progWcC `
  '\tconst RECTANGLE_16\* clippingList = region16_rects\(&clippingRects, &nbClipping\);' (@'
	const RECTANGLE_16* clippingList = region16_rects(&clippingRects, &nbClipping);

	/* HmRdp: identify this pass's clip so a tile the tile decode already
	 * composited with the very same clip can skip its copy below (see the patch
	 * note in native/scripts/patch-freerdp.ps1 step 23). */
	surface->hmrdpUpdateClip =
	    hmrdp_clip_hash(progressive->hmrdpMsgSeq, clippingList, nbClipping);
'@) 'surface->hmrdpUpdateClip'

# (i) the composite skips a copy the decode already did.
Patch-Regex $progWcC `
  '\tif \(!freerdp_image_copy_no_overlap\(pDstData, DstFormat, nDstStep, common\.left, common\.top, width,\n\t                                   height, tile->data, progressive->format, tile->stride, nXSrc,\n\t                                   nYSrc, NULL, FREERDP_KEEP_DST_ALPHA\)\)\n\t\treturn 1;\n' (@'
	if (tile->hmrdpCopied && (tile->hmrdpCopyClip == surface->hmrdpUpdateClip))
	{
		/* HmRdp: the tile decode already wrote exactly these pixels with this very
		 * clip - only the damage bookkeeping below is left (see the patch note in
		 * native/scripts/patch-freerdp.ps1 step 23). */
	}
	else if (!freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, common.left, common.top,
	                                        width, height, tile->data, progressive->format,
	                                        tile->stride, nXSrc, nYSrc, NULL,
	                                        FREERDP_KEEP_DST_ALPHA))
		return 1;
	else
	{
		/* HmRdp dev: the pixels this side copied (the worker side counts the same
		 * area into [19]). */
		__atomic_add_fetch(&HmrdpProgStat[20], (unsigned long long)width * (unsigned long long)height,
		                   __ATOMIC_RELAXED);
	}

'@) 'already wrote exactly these pixels'

# Self-check: a here-string that is not terminated on its own line leaks the rest
# of this file into the patched source. Catch that immediately instead of writing
# PowerShell into a C file.
foreach ($f in @($progWcC, $progWcH))
{
	$text = [System.IO.File]::ReadAllText($f)
	if ($text.Contains("'@)"))
	{
		throw "HmRdp step 23: leaked patch text in $f - a here-string terminator is not on its own line"
	}
}
