# 12) Don't re-allocate planar's scratch buffers on every ResetGraphics: they are per-message
#     working memory, so unchanged geometry skips the ~100MB calloc+zeroing (planes[0]!=NULL witnesses allocation).
$planarC = "$Source\libfreerdp\codec\planar.c"
Patch-Regex $planarC '\tcontext->bgr = FALSE;\n\tcontext->maxWidth = PLANAR_ALIGN\(width, 4\);\n' (@'
	context->bgr = FALSE;

	/* HmRdp: the four scratch buffers below are per-message working memory (written before read),
	 * so unchanged geometry skips the ~100MB calloc+zeroing; planes[0]!=NULL means allocated. */
	{
		const UINT32 newWidth = PLANAR_ALIGN(width, 4);
		const UINT32 newHeight = PLANAR_ALIGN(height, 4);
		if ((context->planes[0] != NULL) && (newWidth == context->maxWidth) &&
		    (newHeight == context->maxHeight))
			return TRUE;
	}

	context->maxWidth = PLANAR_ALIGN(width, 4);

'@) 'scratch buffers below are per-message working memory'

