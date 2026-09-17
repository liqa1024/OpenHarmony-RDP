# 8) Progressive: the per-(tile,component) predictor state must start defined.
#    `progressive_tile_new` fills `tile->data` with 0xFF but takes `sign`/`current`
#    straight from malloc, and a DIFFERENCE (or UPGRADE) pass *reads* them before
#    writing. The encoder assumes zero initial state, so leaving the heap contents
#    there makes the reference decoder's output depend on unrelated allocations -
#    which is exactly a divergence against an engine that zero-initialises its own
#    equivalent buffers (and it is not reproducible across processes).
Patch-File "$Source\libfreerdp\codec\progressive.c" @{
  @'
	size_t signLen = (8192ULL + 32ULL) * 3ULL;
	tile->sign = (BYTE*)winpr_aligned_malloc(signLen, 16);
	if (!tile->sign)
		goto fail;

	size_t currentLen = (8192ULL + 32ULL) * 3ULL;
	tile->current = (BYTE*)winpr_aligned_malloc(currentLen, 16);
	if (!tile->current)
		goto fail;
'@ = @'
	size_t signLen = (8192ULL + 32ULL) * 3ULL;
	tile->sign = (BYTE*)winpr_aligned_malloc(signLen, 16);
	if (!tile->sign)
		goto fail;
	memset(tile->sign, 0, signLen);

	size_t currentLen = (8192ULL + 32ULL) * 3ULL;
	tile->current = (BYTE*)winpr_aligned_malloc(currentLen, 16);
	if (!tile->current)
		goto fail;
	memset(tile->current, 0, currentLen);
'@
}

