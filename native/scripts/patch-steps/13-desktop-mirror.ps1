# 13) Let the desktop-mirror surface compose straight into gdi's primary buffer (same format/pitch,
#     1:1 at the origin); a second/offset/scaled surface falls back to the plain copy.
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

/* HmRdp: give the surface its own buffer back, with the pixels it showed; must run while the primary is alive. */
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
		/* Out of memory: keep the (still valid) primary rather than leave the surface bufferless. */
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

/* HmRdp: ids of the surfaces into the primary buffer, collected before the rebuild (returns count). */
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

$resetOld = @'
	if (update)
	{
		WINPR_ASSERT(update->DesktopResize);
		update->DesktopResize(gdi->context);
	}
'@
$resetNew = @'
	/* HmRdp: the primary buffer is about to be rebuilt; remember who is sharing it (pointer stops identifying them once freed). */
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

$resetLoopOld = @'
		memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
	}

	free(pSurfaceIds);
'@
$resetLoopNew = @'
		if (gdi_gfx_id_in(sharedIds, sharedCount, surface->surfaceId))
		{
			/* Shared: if the new primary fits it exactly, compose into it (already 0xFF); otherwise take its own buffer back. */
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

$outUpdateOld = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;
'@
$outUpdateNew = @'
	if (gdi->suppressOutput)
		return CHANNEL_RC_OK;

	/* HmRdp: a shared surface must be the desktop, or it composes like any other (sharing is decided at create). */
	if (gdi_surface_shares_primary(gdi, surface) && !gdi_surface_is_desktop_mirror(gdi, surface))
		gdi_surface_unshare(gdi, surface);
'@
Patch-Block $gdiGfxC $outUpdateOld $outUpdateNew 'or it composes like any other'

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
		/* HmRdp: for a desktop mirror the source *is* the destination, so copying would only move ~20MB/frame through memory. */
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

$createOld = @'
	memset(surface->data, 0xFF, (size_t)surface->scanline * surface->height);
	region16_init(&surface->invalidRegion);
'@
$createNew = @'
	/* HmRdp: one surface that is exactly the desktop *is* the desktop - compose into the primary and skip the copy; another surface drops sharing first. */
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

