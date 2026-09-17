# 12) HmRdp: do not re-allocate the PLANAR codec's scratch on every ResetGraphics.
#
#     `freerdp_bitmap_planar_context_reset` unconditionally (re)allocates and
#     zeroes four buffers sized from the desktop: planesBuffer x4, pTempData x6,
#     deltaPlanesBuffer x4, rlePlanesBuffer x4. On a 3120x2080 desktop that is
#     ~117MB of calloc/zeroing per call, and one ResetGraphics reaches it once per
#     codec set (the rdp context's and the GFX context's), i.e. ~130ms per event -
#     measured on device (141.8ms / 128.6ms) for a codec this client never uses
#     (Progressive / ClearCodec / uncompressed only). See
#     doc_agent/cpu-path.md §3.
#
#     The buffers are per-message working memory (written before read), so when
#     the geometry did not change there is nothing to do. `planes[0] != NULL`
#     witnesses that the previous call allocated them, so a failed allocation
#     still falls through to the real path.
$planarC = "$Source\libfreerdp\codec\planar.c"
Patch-Regex $planarC '\tcontext->bgr = FALSE;\n\tcontext->maxWidth = PLANAR_ALIGN\(width, 4\);\n' (@'
	context->bgr = FALSE;

	/* HmRdp: the four scratch buffers below are per-message working memory - their
	 * content is written before it is read - so when the geometry did not change
	 * they neither have to be re-allocated nor re-zeroed. Upstream does both
	 * unconditionally, which is ~117MB of calloc + zeroing per call on a
	 * 3120x2080 desktop; a single ResetGraphics reaches this once per codec set
	 * (two sets exist) and cost ~130ms per event, for a codec this client never
	 * uses (see doc_agent/cpu-path.md §3). `planes[0] != NULL`
	 * means the previous call allocated them, so a failure still falls through. */
	{
		const UINT32 newWidth = PLANAR_ALIGN(width, 4);
		const UINT32 newHeight = PLANAR_ALIGN(height, 4);
		if ((context->planes[0] != NULL) && (newWidth == context->maxWidth) &&
		    (newHeight == context->maxHeight))
			return TRUE;
	}

	context->maxWidth = PLANAR_ALIGN(width, 4);
'@) 'scratch buffers below are per-message working memory'

