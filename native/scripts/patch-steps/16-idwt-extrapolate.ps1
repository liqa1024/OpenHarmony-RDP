# 16) Extrapolated (reduced) inverse DWT, the one Progressive actually runs: same arithmetic
#     (byte-identical), reordered so inner loops are contiguous; upstream scalar kept as the reference.
$progDwtC = "$Source\libfreerdp\codec\progressive.c"
$progDwtRows = @'
/* HmRdp: reduced (extrapolated) inverse DWT restructured for contiguous inner loops; arithmetic unchanged. */
#define HMRDP_IDWT_ROW_MAX 66

static INLINE void progressive_rfx_idwt_x(const INT16* WINPR_RESTRICT pLowBand, size_t nLowStep,
                                          const INT16* WINPR_RESTRICT pHighBand, size_t nHighStep,
                                          INT16* WINPR_RESTRICT pDstBand, size_t nDstStep,
                                          size_t nLowCount, size_t nHighCount, size_t nDstCount)
{
	/* X0 at step j is the X2 of step j-1, so the whole X2 sequence is one dependency-free run. */
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

		/* Tail: upstream cases with X2 = x0[nHighCount-1], H0 = pH[nHighCount-1]. */
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
	/* Row-major: band row outer, tile columns inner, so every access is contiguous. */
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

$progDwtBlock = @'
static INLINE void progressive_rfx_dwt_2d_decode_block(INT16* WINPR_RESTRICT buffer,
                                                       INT16* WINPR_RESTRICT temp, size_t level)
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

	/* horizontal (LL + HL -> L) */
	progressive_rfx_idwt_x(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);

	/* horizontal (LH + HH -> H) */
	progressive_rfx_idwt_x(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);

	/* vertical (L + H -> LL) */
	progressive_rfx_idwt_y(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH,
	                       nBandL + nBandH);
}

'@

$progDwtExtrapolate = @'
void rfx_dwt_2d_extrapolate_decode(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT temp)
{
	WINPR_ASSERT(buffer);
	WINPR_ASSERT(temp);

	progressive_rfx_dwt_2d_decode_block(&buffer[3807], temp, 3);
	progressive_rfx_dwt_2d_decode_block(&buffer[3007], temp, 2);
	progressive_rfx_dwt_2d_decode_block(&buffer[0], temp, 1);
}
'@

# One replacement for the whole region; the marker below is what makes a re-run a no-op.
Patch-Regex $progDwtC ' \* LL3      4015        9x9         81\n \*/.*?\nvoid rfx_dwt_2d_extrapolate_decode\(INT16\* WINPR_RESTRICT buffer, INT16\* WINPR_RESTRICT temp\)\n\{.*?\n\}\n' (" * LL3      4015        9x9         81`n */`n`n" + $progDwtRows + $progDwtBlock + $progDwtExtrapolate) 'HmrdpDwtExtrapolateReference'

