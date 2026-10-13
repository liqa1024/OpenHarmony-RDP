# 19) One fewer 8KB pass per decoded tile/component: RLGR decodes straight into `sign` and the
#     dequantise pass reads `sign`/writes `buffer`, removing the copy (bit-exact; LL3 stays in place).
$progStateC = "$Source\libfreerdp\codec\progressive.c"
$progStateNew = @'
/* HmRdp: shift one subband from src into dst; the in-place primitive no-ops for shift 0 or >=16, so those copy. */
static INLINE void progressive_rfx_decode_block_copy(const primitives_t* prims,
                                                     const INT16* WINPR_RESTRICT src,
                                                     INT16* WINPR_RESTRICT dst, UINT32 length,
                                                     UINT32 shift)
{
	if ((shift == 0) || (shift >= 16))
	{
		CopyMemory(dst, src, (size_t)length * sizeof(INT16));
		return;
	}

	prims->lShiftC_16s(src, shift, dst, length);
}

/* HmRdp: in-place variant stays: LL3 is dequantised after its differential decode. */
static INLINE void progressive_rfx_decode_block(const primitives_t* prims,
                                                INT16* WINPR_RESTRICT buffer, UINT32 length,
                                                UINT32 shift)
{
	if (!shift)
		return;

	prims->lShiftC_16s_inplace(buffer, shift, length);
}

static INLINE int progressive_rfx_decode_component(
    PROGRESSIVE_CONTEXT* WINPR_RESTRICT progressive,
    const RFX_COMPONENT_CODEC_QUANT* WINPR_RESTRICT shift, const BYTE* WINPR_RESTRICT data,
    UINT32 length, INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT current,
    INT16* WINPR_RESTRICT sign, BOOL coeffDiff, BOOL subbandDiff, BOOL extrapolate)
{
	int status = 0;
	const primitives_t* prims = primitives_get();

	/* HmRdp: raw coefficients go straight into `sign`, so the pass below dequantises sign -> buffer (copy gone). */
	status = progressive->rfx_context->rlgr_decode(RLGR1, data, length, sign, 4096);

	if (status < 0)
		return status;

	if (!extrapolate)
	{
		CopyMemory(&buffer[4032], &sign[4032], 64ULL * sizeof(INT16));
		rfx_differential_decode(buffer + 4032, 64);
		progressive_rfx_decode_block_copy(prims, &sign[0], &buffer[0], 1024, shift->HL1);    /* HL1 */
		progressive_rfx_decode_block_copy(prims, &sign[1024], &buffer[1024], 1024, shift->LH1); /* LH1 */
		progressive_rfx_decode_block_copy(prims, &sign[2048], &buffer[2048], 1024, shift->HH1); /* HH1 */
		progressive_rfx_decode_block_copy(prims, &sign[3072], &buffer[3072], 256, shift->HL2);  /* HL2 */
		progressive_rfx_decode_block_copy(prims, &sign[3328], &buffer[3328], 256, shift->LH2);  /* LH2 */
		progressive_rfx_decode_block_copy(prims, &sign[3584], &buffer[3584], 256, shift->HH2);  /* HH2 */
		progressive_rfx_decode_block_copy(prims, &sign[3840], &buffer[3840], 64, shift->HL3);   /* HL3 */
		progressive_rfx_decode_block_copy(prims, &sign[3904], &buffer[3904], 64, shift->LH3);   /* LH3 */
		progressive_rfx_decode_block_copy(prims, &sign[3968], &buffer[3968], 64, shift->HH3);   /* HH3 */
		progressive_rfx_decode_block(prims, &buffer[4032], 64, shift->LL3);   /* LL3 */
	}
	else
	{
		progressive_rfx_decode_block_copy(prims, &sign[0], &buffer[0], 1023, shift->HL1);    /* HL1 */
		progressive_rfx_decode_block_copy(prims, &sign[1023], &buffer[1023], 1023, shift->LH1); /* LH1 */
		progressive_rfx_decode_block_copy(prims, &sign[2046], &buffer[2046], 961, shift->HH1);  /* HH1 */
		progressive_rfx_decode_block_copy(prims, &sign[3007], &buffer[3007], 272, shift->HL2);  /* HL2 */
		progressive_rfx_decode_block_copy(prims, &sign[3279], &buffer[3279], 272, shift->LH2);  /* LH2 */
		progressive_rfx_decode_block_copy(prims, &sign[3551], &buffer[3551], 256, shift->HH2);  /* HH2 */
		progressive_rfx_decode_block_copy(prims, &sign[3807], &buffer[3807], 72, shift->HL3);   /* HL3 */
		progressive_rfx_decode_block_copy(prims, &sign[3879], &buffer[3879], 72, shift->LH3);   /* LH3 */
		progressive_rfx_decode_block_copy(prims, &sign[3951], &buffer[3951], 64, shift->HH3);   /* HH3 */
		CopyMemory(&buffer[4015], &sign[4015], 81ULL * sizeof(INT16));
		rfx_differential_decode(&buffer[4015], 81);                           /* LL3 */
		progressive_rfx_decode_block(prims, &buffer[4015], 81, shift->LL3);   /* LL3 */
	}
	return progressive_rfx_dwt_2d_decode(progressive, buffer, current, coeffDiff, extrapolate,
	                                     FALSE);
}

static INLINE int
progressive_decompress_tile_first(
'@
Patch-Regex $progStateC 'static INLINE void progressive_rfx_decode_block\(const primitives_t\* prims,.*?\nstatic INLINE int\nprogressive_decompress_tile_first\(' $progStateNew 'progressive_rfx_decode_block_copy'

