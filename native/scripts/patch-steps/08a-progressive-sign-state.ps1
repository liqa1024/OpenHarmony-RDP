# 8) Progressive predictor state must start defined: tile_new fills data with 0xFF but
#    takes sign/current from malloc, and a DIFFERENCE/UPGRADE pass reads them before writing.
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

