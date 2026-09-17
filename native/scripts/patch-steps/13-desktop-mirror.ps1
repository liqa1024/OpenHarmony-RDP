# 13) HmRdp: let the desktop-mirror surface compose straight into the primary buffer.
#
#     A full-screen GFX session has exactly one surface, mapped to the output at
#     (0,0) with no scaling, whose format and row pitch equal gdi's primary buffer.
#     That surface *is* the desktop, so gdi_OutputUpdate's per-rect
#     freerdp_image_scale() into the primary just moves ~20MB/frame from one buffer
#     to another - measured 1.3ms/frame and 41MB of DRAM traffic per frame on a
#     3120x2080 stream (doc_agent/cpu-path.md §4). With this step the surface is
#     given the primary buffer instead, so the decoder writes the very pixels the
#     presenter uploads and the copy disappears (cpu-path.md §4).
#
#     The sharing test is `surface->data == gdi->primary_buffer`. It stays valid
#     because the only path that replaces the primary buffer (gdi_ResetGraphics ->
#     update->DesktopResize -> gdi_resize) re-points or unshares every shared
#     surface *before* the old buffer goes away, and DeleteSurface never frees a
#     shared one. Anything that does not hold exactly - a second surface, an
#     offset or scaled mapping, a different format/pitch - falls back to the plain
#     copy, so the worst case is the behaviour we had before.
$gdiGfxC = "$Source\libfreerdp\gdi\gfx.c"

$shareHelpers = @'
/* HmRdp: is this surface composing into the primary buffer itself? */
static BOOL gdi_surface_shares_primary(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	return (gdi != NULL) && (surface != NULL) && (gdi->primary_buffer != NULL) &&
	       (surface->data == gdi->primary_buffer);
}

/* HmRdp: byte-identical layout to the desktop, without requiring the mapping. */
static BOOL gdi_surface_matches_geometry(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	if ((gdi == NULL) || (surface == NULL) || (gdi->primary_buffer == NULL))
		return FALSE;
	return (surface->format == gdi->dstFormat) && (surface->width == (UINT32)gdi->width) &&
	       (surface->height == (UINT32)gdi->height) && (surface->scanline == gdi->stride);
}

/* HmRdp: and mapped 1:1 over the whole output at the origin, i.e. the desktop. */
static BOOL gdi_surface_is_desktop_mirror(const rdpGdi* gdi, const gdiGfxSurface* surface)
{
	if (!gdi_surface_matches_geometry(gdi, surface))
		return FALSE;
	if (!surface->outputMapped || (surface->outputOriginX != 0) || (surface->outputOriginY != 0))
		return FALSE;
	if ((surface->mappedWidth != (UINT32)gdi->width) ||
	    (surface->mappedHeight != (UINT32)gdi->height))
		return FALSE;
	return (surface->outputTargetWidth == surface->mappedWidth) &&
	       (surface->outputTargetHeight == surface->mappedHeight);
}

/* HmRdp: give the surface its own buffer back, carrying the pixels it showed. Must
 * run while the shared primary buffer is still alive. A sharing surface is never
 * freed (gdi_DeleteSurface), so only this allocates. */
static void gdi_surface_unshare(rdpGdi* gdi, gdiGfxSurface* surface)
{
	size_t bytes = 0;
	BYTE* own = NULL;
	UINT32 rows = 0;
	UINT32 cols = 0;
	UINT32 y = 0;

	if (!gdi_surface_shares_primary(gdi, surface))
		return;

	bytes = (size_t)surface->scanline * surface->height;
	if (bytes == 0)
		return;

	own = (BYTE*)winpr_aligned_malloc(bytes, 16);
	if (own == NULL)
	{
		/* Out of memory: keep the (still valid) primary rather than leave the
		 * surface without a buffer at all. */
		WLog_ERR(TAG, "HmRdp: cannot unshare the desktop-mirror surface");
		return;
	}
	memset(own, 0xFF, bytes);
	rows = MIN((UINT32)surface->height, (UINT32)gdi->height);
	cols = MIN(surface->scanline, gdi->stride);
	for (y = 0; y < rows; y++)
		memcpy(&own[(size_t)y * surface->scanline], &gdi->primary_buffer[(size_t)y * gdi->stride],
		       cols);
	surface->data = own;
}

/* HmRdp: every surface that currently composes into the primary buffer. */
static void gdi_gfx_unshare_all(RdpgfxClientContext* context, rdpGdi* gdi)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	UINT32 i = 0;

	if ((context == NULL) || (context->GetSurfaceIds == NULL) ||
	    (context->GetSurfaceData == NULL))
		return;
	context->GetSurfaceIds(context, &ids, &count);
	for (i = 0; i < count; i++)
	{
		gdiGfxSurface* surface =
		    (gdiGfxSurface*)context->GetSurfaceData(context, ids[i]);
		if (gdi_surface_shares_primary(gdi, surface))
			gdi_surface_unshare(gdi, surface);
	}
	free(ids);
}

static UINT32 gdi_gfx_surface_count(RdpgfxClientContext* context)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	if ((context == NULL) || (context->GetSurfaceIds == NULL))
		return 0;
	context->GetSurfaceIds(context, &ids, &count);
	free(ids);
	return count;
}

/* HmRdp: the ids of the surfaces that compose into the primary buffer, collected
 * before the primary is rebuilt (afterwards the pointer no longer identifies
 * them). Returns how many were written. */
static UINT32 gdi_gfx_collect_shared(RdpgfxClientContext* context, rdpGdi* gdi, UINT16* out,
                                     UINT32 capacity)
{
	UINT16 count = 0;
	UINT16* ids = NULL;
	UINT32 found = 0;
	UINT32 i = 0;

	if ((out == NULL) || (capacity == 0) || (context == NULL) ||
	    (context->GetSurfaceIds == NULL) || (context->GetSurfaceData == NULL))
		return 0;
	context->GetSurfaceIds(context, &ids, &count);
	for (i = 0; i < count && found < capacity; i++)
	{
		gdiGfxSurface* surface =
		    (gdiGfxSurface*)context->GetSurfaceData(context, ids[i]);
		if (gdi_surface_shares_primary(gdi, surface))
			out[found++] = ids[i];
	}
	free(ids);
	return found;
}

static BOOL gdi_gfx_id_in(const UINT16* ids, UINT32 count, UINT16 id)
{
	UINT32 i = 0;
	for (i = 0; i < count; i++)
	{
		if (ids[i] == id)
			return TRUE;
	}
	return FALSE;
}

'@

$resetDoc = @'
/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT gdi_ResetGraphics(RdpgfxClientContext* context,
'@
Patch-Block $gdiGfxC $resetDoc ($shareHelpers + $resetDoc) `
  'HmRdp: is this surface composing into the primary buffer itself?'

# The primary buffer is rebuilt below (DesktopResize -> gdi_resize), so note which
# surfaces are composing into it while the pointer still identifies them.
$resetOld = @'
	if (update)
	{
		WINPR_ASSERT(update->DesktopResize);
		update->DesktopResize(gdi->context);
	}
'@
$resetNew = @'
	/* HmRdp: the primary buffer is about to be rebuilt; remember who is sharing it
	 * (the pointer stops identifying them once it is freed). */
	UINT16 sharedIds[64] = { 0 };
	UINT32 sharedCount = gdi_gfx_collect_shared(context, gdi, sharedIds,
	                                            (UINT32)(sizeof(sharedIds) / sizeof(sharedIds[0])));

	if (update)
	{
		WINPR_ASSERT(update->DesktopResize);
		update->DesktopResize(gdi->context);
	}
'@
Patch-Block $gdiGfxC $resetOld $resetNew 'HmRdp: the primary buffer is about to be rebuilt'

# Re-point the surfaces that still match the (new) desktop; the rest get their own
# buffer back. Either way the 0xFF reset below only ever touches their own memory.
$resetLoopOld = @'
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
	}

	free(pSurfaceIds);
'@
$resetLoopNew = @'
		if (gdi_gfx_id_in(sharedIds, sharedCount, surface->surfaceId))
		{
			/* Shared: either the new primary fits it exactly (compose into it from
			 * now on - it is already 0xFF, so no reset) or it takes its own buffer
			 * back and is reset like any other surface. */
			if (gdi_surface_matches_geometry(gdi, surface))
			{
				surface->data = gdi->primary_buffer;
				region16_clear(&surface->invalidRegion);
				continue;
			}
			gdi_surface_unshare(gdi, surface);
		}
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
	}

	free(pSurfaceIds);
'@
Patch-Block $gdiGfxC $resetLoopOld $resetLoopNew 'Shared: either the new primary fits it exactly'

# A mapping that is not the full desktop 1:1 takes the buffer back before the copy.
$outUpdateOld = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;
'@
$outUpdateNew = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;

	/* HmRdp: a shared surface must be the desktop, or it composes like any other
	 * (the sharing is decided when the surface is created, before the mapping that
	 * makes it a mirror is known). */
	if (gdi_surface_shares_primary(gdi, surface) && !gdi_surface_is_desktop_mirror(gdi, surface))
		gdi_surface_unshare(gdi, surface);
'@
Patch-Block $gdiGfxC $outUpdateOld $outUpdateNew 'or it composes like any other'

# ... and the copy itself is skipped when the pixels are already in place.
$scaleOld = @'
		if (!freerdp_image_scale(gdi->primary_buffer, gdi->dstFormat, gdi->stride, nXDst, nYDst,
		                         dwidth, dheight, surface->data, surface->format, surface->scanline,
		                         nXSrc, nYSrc, swidth, sheight))
		{
			rc = CHANNEL_RC_NULL_DATA;
			goto fail;
		}
'@
$scaleNew = @'
		/* HmRdp: the source *is* the destination for a desktop mirror, so copying it
		 * would only move ~20MB/frame through memory (the invalid region below is
		 * still what tells the presenter what changed). */
		if (!gdi_surface_shares_primary(gdi, surface))
		{
			if (!freerdp_image_scale(gdi->primary_buffer, gdi->dstFormat, gdi->stride, nXDst, nYDst,
			                         dwidth, dheight, surface->data, surface->format, surface->scanline,
			                         nXSrc, nYSrc, swidth, sheight))
			{
				rc = CHANNEL_RC_NULL_DATA;
				goto fail;
			}
		}
'@
Patch-Block $gdiGfxC $scaleOld $scaleNew 'would only move ~20MB/frame through memory'

# A lone, desktop-sized surface becomes the primary buffer itself.
$createOld = @'
	memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
	region16_init(&surface->invalidRegion);
'@
$createNew = @'
	/* HmRdp: one surface that is exactly the desktop *is* the desktop - compose
	 * into the primary buffer and skip the per-frame copy. Another surface means
	 * the output is no longer a single mirror, so any sharing is dropped first (the
	 * composite would otherwise overwrite the shared surface's own pixels). */
	gdi_gfx_unshare_all(context, gdi);
	if ((gdi_gfx_surface_count(context) == 0) && gdi_surface_matches_geometry(gdi, surface))
	{
		winpr_aligned_free(surface->data);
		surface->data = gdi->primary_buffer;
	}
	else
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
	region16_init(&surface->invalidRegion);
'@
Patch-Block $gdiGfxC $createOld $createNew 'one surface that is exactly the desktop *is* the desktop'

# DeleteSurface must not free the primary buffer.
$deleteDeclOld = @'
	UINT rc = CHANNEL_RC_OK;
	UINT res = ERROR_INTERNAL_ERROR;
	rdpCodecs* codecs = NULL;
	gdiGfxSurface* surface = NULL;
'@
$deleteDeclNew = @'
	UINT rc = CHANNEL_RC_OK;
	UINT res = ERROR_INTERNAL_ERROR;
	rdpCodecs* codecs = NULL;
	gdiGfxSurface* surface = NULL;
	rdpGdi* gdi = (rdpGdi*)context->custom;
'@
Patch-Block $gdiGfxC $deleteDeclOld $deleteDeclNew "`tgdiGfxSurface* surface = NULL;`r`n`trdpGdi* gdi = (rdpGdi*)context->custom;"

$deleteOld = @'
		region16_uninit(&surface->invalidRegion);
		codecs = surface->codecs;
		winpr_aligned_free(surface->data);
		free(surface);
'@
$deleteNew = @'
		region16_uninit(&surface->invalidRegion);
		codecs = surface->codecs;
		/* HmRdp: a shared surface's buffer belongs to gdi's primary, not to it. */
		if (!gdi_surface_shares_primary(gdi, surface))
			winpr_aligned_free(surface->data);
		surface->data = NULL;
		free(surface);
'@
Patch-Block $gdiGfxC $deleteOld $deleteNew 'a shared surface''s buffer belongs to gdi''s primary'

