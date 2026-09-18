# 16) HmRdp: the extrapolated (reduced) inverse DWT - the one the Progressive
#     codec actually runs (doc_agent/gfx-engine.md 8.2).
#
#     `rfx_dwt_2d_extrapolate_decode` (below) is what a region with the
#     RFX_DWT_REDUCE_EXTRAPOLATE flag decodes with, and every tile of a
#     full-screen Progressive stream carries that flag (measured: the
#     non-extrapolated rfx_dwt_2d_decode() is never entered on a video capture).
#     It walks the bands with a recurrence in the *inner* loop:
#
#       X2 = L0 - (H0 + H1) / 2;   X1 = (X0 + X2) / 2 + 2 * H0;   X0 = X2;
#
#     so every sample of one column depends on the previous one. The rewrite
#     below changes nothing about that arithmetic - the INT16 truncation on every
#     assignment and the truncating `/ 2` are the original ones, so the output is
#     byte-for-byte identical - it only reorders the work:
#
#       - the X2 sequence is a plain function of the L/H samples, so it is
#         computed first (idwt_x into a scratch row, idwt_y into one scratch row
#         per column) and the two output samples per step are then written by a
#         dependency-free loop;
#       - idwt_y becomes row-major: j (the band row) is the outer loop and the
#         inner loop runs over the tile columns, so both the reads and the writes
#         are contiguous. The upstream version walks *columns*, i.e. every single
#         sample lands in a different cache line (128-byte stride), which is why
#         it costs more than its arithmetic suggests.
#
#     The upstream scalar functions are kept verbatim as `..._scalar` and are the
#     reference for the dev comparison (the extrapolate entry point runs it on a
#     sample of tiles; see step 15 for the counters). Everything is plain C - no
#     intrinsics - so the scalar build for other targets keeps working unchanged.
$progDwtC = "$Source\libfreerdp\codec\progressive.c"
$progDwtRows = @'
/*
 * HmRdp: the reduced (extrapolated) inverse DWT, restructured so its inner loops
 * are contiguous and dependency-free; the arithmetic is the upstream one (see
 * the patch note in native/scripts/patch-freerdp.ps1 step 16). The upstream
 * scalar version follows first and stays the reference for the dev comparison.
 */
#define HMRDP_IDWT_ROW_MAX 66

static INLINE void progressive_rfx_idwt_x_scalar(const INT16* WINPR_RESTRICT pLowBand,
                                                 size_t nLowStep,
                                                 const INT16* WINPR_RESTRICT pHighBand,
                                                 size_t nHighStep, INT16* WINPR_RESTRICT pDstBand,
                                                 size_t nDstStep, size_t nLowCount,
                                                 size_t nHighCount, size_t nDstCount)
{
	INT16 L0 = 0;
	INT16 H0 = 0;
	INT16 H1 = 0;
	INT16 X0 = 0;
	INT16 X1 = 0;
	INT16 X2 = 0;

	for (size_t i = 0; i < nDstCount; i++)
	{
		const INT16* pL = pLowBand;
		const INT16* pH = pHighBand;
		INT16* pX = pDstBand;
		H0 = *pH++;
		L0 = *pL++;
		X0 = L0 - H0;
		X2 = L0 - H0;

		for (size_t j = 0; j < (nHighCount - 1); j++)
		{
			H1 = *pH;
			pH++;
			L0 = *pL;
			pL++;
			X2 = L0 - ((H0 + H1) / 2);
			X1 = ((X0 + X2) / 2) + (2 * H0);
			pX[0] = X0;
			pX[1] = X1;
			pX += 2;
			X0 = X2;
			H0 = H1;
		}

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				pX[0] = X2;
				pX[1] = X2 + (2 * H0);
			}
			else
			{
				L0 = *pL;
				pL++;
				X0 = L0 - H0;
				pX[0] = X2;
				pX[1] = ((X0 + X2) / 2) + (2 * H0);
				pX[2] = X0;
			}
		}
		else
		{
			L0 = *pL;
			pL++;
			X0 = L0 - (H0 / 2);
			pX[0] = X2;
			pX[1] = ((X0 + X2) / 2) + (2 * H0);
			pX[2] = X0;
			L0 = *pL;
			pL++;
			pX[3] = (X0 + L0) / 2;
		}

		pLowBand += nLowStep;
		pHighBand += nHighStep;
		pDstBand += nDstStep;
	}
}

static INLINE void progressive_rfx_idwt_y_scalar(const INT16* WINPR_RESTRICT pLowBand,
                                                 size_t nLowStep,
                                                 const INT16* WINPR_RESTRICT pHighBand,
                                                 size_t nHighStep, INT16* WINPR_RESTRICT pDstBand,
                                                 size_t nDstStep, size_t nLowCount,
                                                 size_t nHighCount, size_t nDstCount)
{
	INT16 L0 = 0;
	INT16 H0 = 0;
	INT16 H1 = 0;
	INT16 X0 = 0;
	INT16 X1 = 0;
	INT16 X2 = 0;

	for (size_t i = 0; i < nDstCount; i++)
	{
		const INT16* pL = pLowBand;
		const INT16* pH = pHighBand;
		INT16* pX = pDstBand;
		H0 = *pH;
		pH += nHighStep;
		L0 = *pL;
		pL += nLowStep;
		X0 = L0 - H0;
		X2 = L0 - H0;

		for (size_t j = 0; j < (nHighCount - 1); j++)
		{
			H1 = *pH;
			pH += nHighStep;
			L0 = *pL;
			pL += nLowStep;
			X2 = L0 - ((H0 + H1) / 2);
			X1 = ((X0 + X2) / 2) + (2 * H0);
			*pX = X0;
			pX += nDstStep;
			*pX = X1;
			pX += nDstStep;
			X0 = X2;
			H0 = H1;
		}

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				*pX = X2;
				pX += nDstStep;
				*pX = X2 + (2 * H0);
			}
			else
			{
				L0 = *pL;
				X0 = L0 - H0;
				*pX = X2;
				pX += nDstStep;
				*pX = ((X0 + X2) / 2) + (2 * H0);
				pX += nDstStep;
				*pX = X0;
			}
		}
		else
		{
			L0 = *pL;
			pL += nLowStep;
			X0 = L0 - (H0 / 2);
			*pX = X2;
			pX += nDstStep;
			*pX = ((X0 + X2) / 2) + (2 * H0);
			pX += nDstStep;
			*pX = X0;
			pX += nDstStep;
			L0 = *pL;
			*pX = (X0 + L0) / 2;
		}

		pLowBand++;
		pHighBand++;
		pDstBand++;
	}
}

static INLINE void progressive_rfx_idwt_x(const INT16* WINPR_RESTRICT pLowBand, size_t nLowStep,
                                          const INT16* WINPR_RESTRICT pHighBand, size_t nHighStep,
                                          INT16* WINPR_RESTRICT pDstBand, size_t nDstStep,
                                          size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
	/* X0 at step j (the even sample written at 2j) is the X2 of step j - 1, so
	 * the whole X2 sequence is one dependency-free run over the bands; the pairs
	 * are then written by a second run. */
	INT16 x0[HMRDP_IDWT_ROW_MAX + 1];
	size_t i = 0;
	size_t j = 0;

	for (i = 0; i < nDstCount; i++)
	{
		const INT16* WINPR_RESTRICT pL = pLowBand;
		const INT16* WINPR_RESTRICT pH = pHighBand;
		INT16* WINPR_RESTRICT pX = pDstBand;

		x0[0] = (INT16)(pL[0] - pH[0]);
		for (j = 0; j + 1 < nHighCount; j++)
			x0[j + 1] = (INT16)(pL[j + 1] - (((int)pH[j] + (int)pH[j + 1]) / 2));

		for (j = 0; j + 1 < nHighCount; j++)
		{
			pX[0] = x0[j];
			pX[1] = (INT16)((((int)x0[j] + (int)x0[j + 1]) / 2) + (2 * (int)pH[j]));
			pX += 2;
		}

		/* The tail keeps the upstream cases, with X2 = x0[nHighCount - 1] and
		 * H0 = pH[nHighCount - 1] (what the scalar loop leaves behind). */
		{
			const INT16 H0 = pH[nHighCount - 1];
			const INT16 X2 = x0[nHighCount - 1];

			if (nLowCount <= (nHighCount + 1))
			{
				if (nLowCount <= nHighCount)
				{
					pX[0] = X2;
					pX[1] = (INT16)(X2 + (2 * (int)H0));
				}
				else
				{
					const INT16 X0 = (INT16)(pL[nHighCount] - H0);
					pX[0] = X2;
					pX[1] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)H0));
					pX[2] = X0;
				}
			}
			else
			{
				const INT16 X0 = (INT16)(pL[nHighCount] - (H0 / 2));
				pX[0] = X2;
				pX[1] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)H0));
				pX[2] = X0;
				pX[3] = (INT16)(((int)X0 + (int)pL[nHighCount + 1]) / 2);
			}
		}

		pLowBand += nLowStep;
		pHighBand += nHighStep;
		pDstBand += nDstStep;
	}
}

static INLINE void progressive_rfx_idwt_y(const INT16* WINPR_RESTRICT pLowBand, size_t nLowStep,
                                          const INT16* WINPR_RESTRICT pHighBand, size_t nHighStep,
                                          INT16* WINPR_RESTRICT pDstBand, size_t nDstStep,
                                          size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
	/* Row-major: the band row is the outer loop and the tile columns are the
	 * inner one, so every access is a contiguous run. The recurrence only needs
	 * the previous X2 row, which the swap keeps. */
	INT16 rowA[HMRDP_IDWT_ROW_MAX + 1];
	INT16 rowB[HMRDP_IDWT_ROW_MAX + 1];
	INT16* WINPR_RESTRICT prev = rowA;
	INT16* WINPR_RESTRICT cur = rowB;
	size_t i = 0;
	size_t j = 0;

	for (i = 0; i < nDstCount; i++)
		prev[i] = (INT16)(pLowBand[i] - pHighBand[i]);

	for (j = 0; j + 1 < nHighCount; j++)
	{
		const INT16* WINPR_RESTRICT lj = pLowBand + (j + 1) * nLowStep;
		const INT16* WINPR_RESTRICT hj = pHighBand + j * nHighStep;
		INT16* WINPR_RESTRICT o0 = pDstBand + (j * 2) * nDstStep;
		INT16* WINPR_RESTRICT o1 = o0 + nDstStep;

		for (i = 0; i < nDstCount; i++)
			cur[i] = (INT16)(lj[i] - (((int)hj[i] + (int)hj[nHighStep + i]) / 2));

		for (i = 0; i < nDstCount; i++)
		{
			o0[i] = prev[i];
			o1[i] = (INT16)((((int)prev[i] + (int)cur[i]) / 2) + (2 * (int)hj[i]));
		}

		{
			INT16* WINPR_RESTRICT swap = prev;
			prev = cur;
			cur = swap;
		}
	}

	/* Tail: X2 = prev, H0 = the last high-band row. */
	{
		const INT16* WINPR_RESTRICT h0 = pHighBand + (nHighCount - 1) * nHighStep;
		INT16* WINPR_RESTRICT o = pDstBand + (2 * (nHighCount - 1)) * nDstStep;

		if (nLowCount <= (nHighCount + 1))
		{
			if (nLowCount <= nHighCount)
			{
				for (i = 0; i < nDstCount; i++)
				{
					o[i] = prev[i];
					o[nDstStep + i] = (INT16)(prev[i] + (2 * (int)h0[i]));
				}
			}
			else
			{
				const INT16* WINPR_RESTRICT ln = pLowBand + nHighCount * nLowStep;
				for (i = 0; i < nDstCount; i++)
				{
					const INT16 X0 = (INT16)(ln[i] - h0[i]);
					const INT16 X2 = prev[i];
					o[i] = X2;
					o[nDstStep + i] =
					    (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)h0[i]));
					o[2 * nDstStep + i] = X0;
				}
			}
		}
		else
		{
			const INT16* WINPR_RESTRICT ln = pLowBand + nHighCount * nLowStep;
			for (i = 0; i < nDstCount; i++)
			{
				const INT16 X0 = (INT16)(ln[i] - (h0[i] / 2));
				const INT16 X2 = prev[i];
				const INT16 L1 = ln[nLowStep + i];
				o[i] = X2;
				o[nDstStep + i] = (INT16)((((int)X0 + (int)X2) / 2) + (2 * (int)h0[i]));
				o[2 * nDstStep + i] = X0;
				o[3 * nDstStep + i] = (INT16)(((int)X0 + (int)L1) / 2);
			}
		}
	}
}

static INLINE size_t progressive_rfx_get_band_l_count(size_t level)
{
	return (64 >> level) + 1;
}

static INLINE size_t progressive_rfx_get_band_h_count(size_t level)
{
	if (level == 1)
		return (64 >> 1) - 1;
	else
		return (64 + (1 << (level - 1))) >> level;
}

'@

# ... the block wrapper learns a "scalar" switch (used by the dev comparison).
$progDwtBlock = @'
static INLINE void progressive_rfx_dwt_2d_decode_block_mode(INT16* WINPR_RESTRICT buffer,
                                                            INT16* WINPR_RESTRICT temp, size_t level,
                                                            BOOL scalar)
{
	size_t nDstStepX = 0;
	size_t nDstStepY = 0;
	const INT16* WINPR_RESTRICT HL = NULL;
	const INT16* WINPR_RESTRICT LH = NULL;
	const INT16* WINPR_RESTRICT HH = NULL;
	INT16* WINPR_RESTRICT LL = NULL;
	INT16* WINPR_RESTRICT L = NULL;
	INT16* WINPR_RESTRICT H = NULL;
	INT16* WINPR_RESTRICT LLx = NULL;

	const size_t nBandL = progressive_rfx_get_band_l_count(level);
	const size_t nBandH = progressive_rfx_get_band_h_count(level);
	size_t offset = 0;

	HL = &buffer[offset];
	offset += (nBandH * nBandL);
	LH = &buffer[offset];
	offset += (nBandL * nBandH);
	HH = &buffer[offset];
	offset += (nBandH * nBandH);
	LL = &buffer[offset];
	nDstStepX = (nBandL + nBandH);
	nDstStepY = (nBandL + nBandH);
	offset = 0;
	L = &temp[offset];
	offset += (nBandL * nDstStepX);
	H = &temp[offset];
	LLx = &buffer[0];

	if (scalar)
	{
		/* horizontal (LL + HL -> L) */
		progressive_rfx_idwt_x_scalar(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);

		/* horizontal (LH + HH -> H) */
		progressive_rfx_idwt_x_scalar(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);

		/* vertical (L + H -> LL) */
		progressive_rfx_idwt_y_scalar(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH,
		                              nBandL + nBandH);
		return;
	}

	/* horizontal (LL + HL -> L) */
	progressive_rfx_idwt_x(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);

	/* horizontal (LH + HH -> H) */
	progressive_rfx_idwt_x(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);

	/* vertical (L + H -> LL) */
	progressive_rfx_idwt_y(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH,
	                       nBandL + nBandH);
}

static INLINE void progressive_rfx_dwt_2d_decode_block(INT16* WINPR_RESTRICT buffer,
                                                       INT16* WINPR_RESTRICT temp, size_t level)
{
	progressive_rfx_dwt_2d_decode_block_mode(buffer, temp, level, FALSE);
}

'@

# ... and the entry point runs the dev comparison on a sample of tiles.
$progDwtExtrapolate = @'
/* Exported by the patched rfx_dwt.c (step 15): the dev comparison of the
 * inverse DWT against its scalar reference. Armed for one tile in 16 of the
 * reference runs - the reduced DWT is the one a Progressive region actually
 * uses, so this is where the sample has to be taken. */
extern int HmrdpDwtCheckArmed(void);
extern void HmrdpDwtCheckResult(unsigned int bad, int maxDelta);

static void hmrdp_dwt_2d_decode_extrapolate_mode(INT16* WINPR_RESTRICT buffer,
                                                 INT16* WINPR_RESTRICT temp, BOOL scalar)
{
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[3807], temp, 3, scalar);
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[3007], temp, 2, scalar);
	progressive_rfx_dwt_2d_decode_block_mode(&buffer[0], temp, 1, scalar);
}

/* The bit-exact reference of this entry point, for whichever implementation is
 * wired up to it (the NEON variant in codec/neon/rfx_neon.c compares through
 * this): the upstream scalar arithmetic, unchanged. */
FREERDP_API void HmrdpDwtExtrapolateReference(INT16* WINPR_RESTRICT buffer,
                                              INT16* WINPR_RESTRICT temp)
{
	hmrdp_dwt_2d_decode_extrapolate_mode(buffer, temp, TRUE);
}

void rfx_dwt_2d_extrapolate_decode(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT temp)
{
	static INT16 ref[4096] = { 0 };
	static INT16 scratch[4096] = { 0 };
	const BOOL check = HmrdpDwtCheckArmed();

	WINPR_ASSERT(buffer);
	WINPR_ASSERT(temp);

	/* HmRdp dev: the decode overwrites the whole coefficient buffer, so the input
	 * is copied first and the scalar reference runs on that copy below. */
	if (check)
		memcpy(ref, buffer, sizeof(ref));

	hmrdp_dwt_2d_decode_extrapolate_mode(buffer, temp, FALSE);

	if (check)
	{
		UINT32 bad = 0;
		int maxDelta = 0;
		size_t i = 0;

		hmrdp_dwt_2d_decode_extrapolate_mode(ref, scratch, TRUE);

		for (i = 0; i < 4096; i++)
		{
			const int delta = (int)buffer[i] - (int)ref[i];
			const int absDelta = (delta < 0) ? -delta : delta;

			if (absDelta != 0)
				bad++;
			if (absDelta > maxDelta)
				maxDelta = absDelta;
		}
		HmrdpDwtCheckResult(bad, maxDelta);
	}
}
'@

# One replacement for the whole region: the `.*?` between the anchors absorbs
# whatever version is in the tree, so re-running over an already patched source
# stays safe (the marker below is what makes it a no-op then).
Patch-Regex $progDwtC ' \* LL3      4015        9x9         81\n \*/.*?\nvoid rfx_dwt_2d_extrapolate_decode\(INT16\* WINPR_RESTRICT buffer, INT16\* WINPR_RESTRICT temp\)\n\{.*?\n\}\n' (" * LL3      4015        9x9         81`n */`n`n" + $progDwtRows + $progDwtBlock + $progDwtExtrapolate) 'HmrdpDwtExtrapolateReference'

