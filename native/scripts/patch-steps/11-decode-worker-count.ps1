# 11) Decode width comes from the app via weak HmrdpDecodeWidth() (1 = serial on the receiving
#     thread); the serial branch is width <= 1, independent of the WinPR pool.
$progC = "$Source\libfreerdp\codec\progressive.c"

Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/* HmRdp: the decode width requested by the app (hmrdp_parallel.*). Weak, so a
 * build without the platform executor decodes on the receiving thread. */
extern unsigned int HmrdpDecodeWidth(void) __attribute__((weak));

static INLINE UINT32 hmrdp_decode_width(void)
{
	return HmrdpDecodeWidth != NULL ? HmrdpDecodeWidth() : 1u;
}

static INLINE SSIZE_T progressive_process_tiles(
'@) 'hmrdp_decode_width'

Patch-Regex $progC '\tif \(!progressive->rfx_context->priv->UseThreads\)\n\t\{\n\t\t/\* Serial: one call per tile, exactly as before the chunking change\. \*/\n\t\tfor \(UINT32 idx = 0; idx < region->numTiles; idx\+\+\)\n\t\t\tprogressive_process_tiles_tile_work_callback\(0, &progressive->params\[idx\], 0\);\n\n\t\tgoto fail;\n\t\}\n' (@'
	if (hmrdp_decode_width() <= 1)
	{
		/* Serial (width 1, or no platform executor): one call per tile, no task
		 * submission. */
		for (UINT32 idx = 0; idx < region->numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);

		goto fail;
	}
'@ + "`n") 'no task submission'
