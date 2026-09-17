# 15) HmRdp: bit-exact inverse DWT (doc_agent/gfx-engine.md 8.2 (single-core C1)).
#
#     The progressive decode spends about half of its tile cost in the inverse
#     DWT, and the upstream block is pure scalar int16 lifting. The rewrite below
#     changes only how the same arithmetic is organised, so the output is
#     byte-for-byte identical to the original (kept verbatim as the scalar
#     reference):
#
#       - the horizontal and the vertical pass are each split into their even- and
#         odd-sample halves - which is what the original already is: an odd sample
#         (2n+1) only ever reads the even samples next to it, never another odd
#         one, so nothing here is a new dependency;
#       - the vertical pass walks rows (x innermost) instead of columns, so every
#         access is contiguous;
#       - on AArch64 the inner loops work on eight samples at a time with the NEON
#         halving adds: VRHADD is (a + b + 1) >> 1 and VHADD is (a + b) >> 1, both
#         with the sum evaluated in full precision - exactly the `int` arithmetic
#         of the C code. The left shift and the final store keep the same INT16
#         truncation, so nothing saturates or rounds differently. (The upstream
#         NEON variant in codec/neon/rfx_neon.c does NOT have this property: it
#         adds in 16-bit lanes and wraps, which is why it stays disabled with
#         -DWITH_SIMD=OFF and is not what this step turns on.)
#
#     `subband_width` is 8, 16 or 32 for every caller, i.e. always a multiple of
#     the vector width; anything else, and every non-AArch64 target (the emulator
#     build), runs the scalar reference unchanged.
#
#     The block also carries the dev-only proof required by that gate: with
#     HmrdpSetDwtCheck(1) one tile in HMRDP_DWT_CHECK_EVERY is decoded a second
#     time with the scalar reference and the differing elements are counted into
#     HmrdpDwtCheckStat[2] = { tiles checked, elements that differed }. The golden
#     reference cannot prove this rewrite on its own (it was recorded by the very
#     decoder that changed), which is why the check exists; the app turns it on
#     for the 参考:对比 run and prints the counters.
$dwtC = "$Source\libfreerdp\codec\rfx_dwt.c"
$dwtNew = @'
/*
 * HmRdp: bit-exact restructure of the inverse DWT (see the patch note in
 * native/scripts/patch-freerdp.ps1 step 15, doc_agent/gfx-engine.md 8.2). The
 * upstream scalar version follows verbatim and stays the reference for the
 * fallback path and for the dev comparison (HmrdpSetDwtCheck).
 */
#define HMRDP_DWT_SUBBAND_MAX 64

static INLINE void hmrdp_dwt_2d_decode_block_scalar(INT16* WINPR_RESTRICT buffer,
                                                    INT16* WINPR_RESTRICT idwt,
                                                    size_t subband_width)
{
	const size_t total_width = subband_width << 1;

	/* Inverse DWT in horizontal direction, results in 2 sub-bands in L, H order in tmp buffer idwt.
	 */
	/* The 4 sub-bands are stored in HL(0), LH(1), HH(2), LL(3) order. */
	/* The lower part L uses LL(3) and HL(0). */
	/* The higher part H uses LH(1) and HH(2). */

	const INT16* ll = buffer + subband_width * subband_width * 3;
	const INT16* hl = buffer;
	INT16* l_dst = idwt;

	const INT16* lh = buffer + subband_width * subband_width;
	const INT16* hh = buffer + subband_width * subband_width * 2;
	INT16* h_dst = idwt + subband_width * subband_width * 2;

	for (size_t y = 0; y < subband_width; y++)
	{
		/* Even coefficients */
		l_dst[0] = ll[0] - ((hl[0] + hl[0] + 1) >> 1);
		h_dst[0] = lh[0] - ((hh[0] + hh[0] + 1) >> 1);
		for (size_t n = 1; n < subband_width; n++)
		{
			const size_t x = n << 1;
			l_dst[x] = ll[n] - ((hl[n - 1] + hl[n] + 1) >> 1);
			h_dst[x] = lh[n] - ((hh[n - 1] + hh[n] + 1) >> 1);
		}

		/* Odd coefficients */
		size_t n = 0;
		for (; n < subband_width - 1; n++)
		{
			const size_t x = n << 1;
			l_dst[x + 1] = (hl[n] << 1) + ((l_dst[x] + l_dst[x + 2]) >> 1);
			h_dst[x + 1] = (hh[n] << 1) + ((h_dst[x] + h_dst[x + 2]) >> 1);
		}

		const size_t x = n << 1;
		l_dst[x + 1] = (hl[n] << 1) + (l_dst[x]);
		h_dst[x + 1] = (hh[n] << 1) + (h_dst[x]);

		ll += subband_width;
		hl += subband_width;
		l_dst += total_width;

		lh += subband_width;
		hh += subband_width;
		h_dst += total_width;
	}

	/* Inverse DWT in vertical direction, results are stored in original buffer. */
	for (size_t x = 0; x < total_width; x++)
	{
		const INT16* l = idwt + x;
		const INT16* h = idwt + x + subband_width * total_width;
		INT16* dst = buffer + x;

		*dst = *l - ((*h * 2 + 1) >> 1);

		for (size_t n = 1; n < subband_width; n++)
		{
			l += total_width;
			h += total_width;

			/* Even coefficients */
			dst[2 * total_width] = *l - ((*(h - total_width) + *h + 1) >> 1);

			/* Odd coefficients */
			dst[total_width] = (*(h - total_width) << 1) + ((*dst + dst[2 * total_width]) >> 1);

			dst += 2 * total_width;
		}

		dst[total_width] = (*h << 1) + ((*dst * 2) >> 1);
	}
}

#if defined(__aarch64__)
#include <arm_neon.h>

#define HMRDP_DWT_LANES 8

/* One row of the horizontal pass: the even samples into a scratch row, then the
 * odd ones, which are written together with their even neighbour (vst2q stores
 * the pair interleaved, i.e. dst[2n] / dst[2n+1] exactly as the scalar code
 * writes them). */
static INLINE void hmrdp_dwt_2d_decode_h_row(const INT16* WINPR_RESTRICT l,
                                             const INT16* WINPR_RESTRICT h,
                                             INT16* WINPR_RESTRICT dst, size_t subband_width)
{
	INT16 even[HMRDP_DWT_SUBBAND_MAX];
	size_t n = 0;

	for (n = 0; n < subband_width; n += HMRDP_DWT_LANES)
	{
		const int16x8_t lv = vld1q_s16(l + n);
		const int16x8_t hv = vld1q_s16(h + n);
		int16x8_t left;

		if (n == 0)
		{
			/* h[n - 1] is h[0] at the row start (the scalar code uses hl[0] twice). */
			left = vextq_s16(hv, hv, HMRDP_DWT_LANES - 1);
			left = vsetq_lane_s16(vgetq_lane_s16(hv, 0), left, 0);
		}
		else
			left = vld1q_s16(h + n - 1);

		vst1q_s16(even + n, vsubq_s16(lv, vrhaddq_s16(left, hv)));
	}

	for (n = 0; n < subband_width; n += HMRDP_DWT_LANES)
	{
		const int16x8_t ev = vld1q_s16(even + n);
		const int16x8_t hv = vld1q_s16(h + n);
		int16x8x2_t pair;
		int16x8_t next;

		if (n + HMRDP_DWT_LANES < subband_width)
			next = vld1q_s16(even + n + 1);
		else
		{
			/* The last odd sample pairs the last even sample with itself. */
			next = vextq_s16(ev, ev, 1);
			next = vsetq_lane_s16(vgetq_lane_s16(ev, HMRDP_DWT_LANES - 1), next,
			                      HMRDP_DWT_LANES - 1);
		}

		pair.val[0] = ev;
		pair.val[1] = vaddq_s16(vshlq_n_s16(hv, 1), vhaddq_s16(ev, next));
		vst2q_s16(dst + 2 * n, pair);
	}
}

/* Vertical pass, one row at a time: the even output rows first (each reads the
 * two high-band rows around it), then the odd rows (each reads the two even rows
 * around it, both of which are already final). */
static INLINE void hmrdp_dwt_2d_decode_v(const INT16* WINPR_RESTRICT l,
                                         const INT16* WINPR_RESTRICT h,
                                         INT16* WINPR_RESTRICT dst, size_t subband_width)
{
	const size_t total_width = subband_width << 1;
	size_t n = 0;
	size_t x = 0;

	for (n = 0; n < subband_width; n++)
	{
		const INT16* WINPR_RESTRICT ln = l + n * total_width;
		const INT16* WINPR_RESTRICT hn = h + n * total_width;
		const INT16* WINPR_RESTRICT hm = (n == 0) ? hn : (hn - total_width);
		INT16* WINPR_RESTRICT d = dst + (n << 1) * total_width;

		for (x = 0; x < total_width; x += HMRDP_DWT_LANES)
			vst1q_s16(d + x, vsubq_s16(vld1q_s16(ln + x),
			                           vrhaddq_s16(vld1q_s16(hm + x), vld1q_s16(hn + x))));
	}

	for (n = 0; n < subband_width; n++)
	{
		const INT16* WINPR_RESTRICT hn = h + n * total_width;
		INT16* WINPR_RESTRICT d0 = dst + (n << 1) * total_width;
		INT16* WINPR_RESTRICT d2 = ((n + 1) < subband_width) ? (d0 + (total_width << 1)) : d0;
		INT16* WINPR_RESTRICT d = d0 + total_width;

		for (x = 0; x < total_width; x += HMRDP_DWT_LANES)
			vst1q_s16(d + x, vaddq_s16(vshlq_n_s16(vld1q_s16(hn + x), 1),
			                           vhaddq_s16(vld1q_s16(d0 + x), vld1q_s16(d2 + x))));
	}
}

static INLINE void hmrdp_dwt_2d_decode_block_neon(INT16* WINPR_RESTRICT buffer,
                                                  INT16* WINPR_RESTRICT idwt,
                                                  size_t subband_width)
{
	const size_t area = subband_width * subband_width;
	const size_t total_width = subband_width << 1;
	size_t y = 0;

	for (y = 0; y < subband_width; y++)
	{
		hmrdp_dwt_2d_decode_h_row(buffer + area * 3 + y * subband_width,
		                          buffer + y * subband_width, idwt + y * total_width,
		                          subband_width);
		hmrdp_dwt_2d_decode_h_row(buffer + area + y * subband_width,
		                          buffer + area * 2 + y * subband_width,
		                          idwt + area * 2 + y * total_width, subband_width);
	}

	hmrdp_dwt_2d_decode_v(idwt, idwt + area * 2, buffer, subband_width);
}
#endif /* __aarch64__ */

static INLINE void rfx_dwt_2d_decode_block(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT idwt,
                                           size_t subband_width)
{
#if defined(__aarch64__)
	if (((subband_width % HMRDP_DWT_LANES) == 0) && (subband_width <= HMRDP_DWT_SUBBAND_MAX))
	{
		hmrdp_dwt_2d_decode_block_neon(buffer, idwt, subband_width);
		return;
	}
#endif
	hmrdp_dwt_2d_decode_block_scalar(buffer, idwt, subband_width);
}

/* ---- HmRdp dev: scalar reference vs the optimised block ------------------- */
/* Off unless the app asks for it (HmrdpSetDwtCheck). The decode is single
 * threaded (the worker count is pinned to one), so plain globals are enough; the
 * counters are read back into the app statistics line. HmrdpDwtCheckArmed() is
 * shared with the extrapolated DWT in progressive.c and with the NEON variants,
 * i.e. with whichever entry point actually runs.
 *
 * HmrdpDwtCheckStat = { tiles compared, elements that differed, worst |delta| }:
 * a rounding-only difference shows up as a small worst delta, a wrap-around or a
 * wrong index shows up as a large one, so the two can be told apart on the same
 * run. */
FREERDP_API unsigned long long HmrdpDwtCheckStat[3] = { 0, 0, 0 };

static volatile LONG g_HmrdpDwtCheck = 0;
static volatile LONG g_HmrdpDwtSample = 0;
static INT16 g_HmrdpDwtRef[4096] = { 0 };
static INT16 g_HmrdpDwtScratch[4096] = { 0 };

#define HMRDP_DWT_CHECK_EVERY 16

FREERDP_API void HmrdpSetDwtCheck(int on)
{
	__atomic_store_n(&g_HmrdpDwtCheck, on ? 1 : 0, __ATOMIC_RELAXED);
}

/* True for one decode in HMRDP_DWT_CHECK_EVERY, and only while the check is on. */
FREERDP_API int HmrdpDwtCheckArmed(void)
{
	if (__atomic_load_n(&g_HmrdpDwtCheck, __ATOMIC_RELAXED) == 0)
		return 0;
	return (__atomic_add_fetch(&g_HmrdpDwtSample, 1, __ATOMIC_RELAXED) % HMRDP_DWT_CHECK_EVERY) == 0;
}

/* Collects one compared tile. */
FREERDP_API void HmrdpDwtCheckResult(unsigned int bad, int maxDelta)
{
	HmrdpDwtCheckStat[0]++;
	HmrdpDwtCheckStat[1] += bad;
	if ((int)HmrdpDwtCheckStat[2] < maxDelta)
		HmrdpDwtCheckStat[2] = (unsigned long long)maxDelta;
}

/* The three levels of the reference implementation: what the dev comparison (in
 * this file, in progressive.c and in the NEON variants) runs on a sampled tile. */
FREERDP_API void HmrdpDwtReference(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT temp)
{
	hmrdp_dwt_2d_decode_block_scalar(&buffer[3840], temp, 8);
	hmrdp_dwt_2d_decode_block_scalar(&buffer[3072], temp, 16);
	hmrdp_dwt_2d_decode_block_scalar(&buffer[0], temp, 32);
}

static void hmrdp_dwt_check(const INT16* buffer)
{
	const size_t elements = 4096;
	size_t i = 0;
	UINT32 bad = 0;
	int maxDelta = 0;

	/* g_HmrdpDwtRef holds the input coefficients, copied before the optimised
	 * decode ran: the decode overwrites the whole coefficient buffer, so the
	 * scalar reference has to start from the same input, not from its result. */
	HmrdpDwtReference(g_HmrdpDwtRef, g_HmrdpDwtScratch);

	for (i = 0; i < elements; i++)
	{
		const int cur = buffer[i];
		const int ref = g_HmrdpDwtRef[i];
		const int delta = (cur > ref) ? (cur - ref) : (ref - cur);

		if (delta != 0)
			bad++;
		if (delta > maxDelta)
			maxDelta = delta;
	}

	HmrdpDwtCheckResult(bad, maxDelta);
}

void rfx_dwt_2d_decode(INT16* WINPR_RESTRICT buffer, INT16* WINPR_RESTRICT dwt_buffer)
{
	const BOOL check = HmrdpDwtCheckArmed();

	WINPR_ASSERT(buffer);
	WINPR_ASSERT(dwt_buffer);

	/* HmRdp dev: take the input before the decode overwrites it, then re-decode
	 * that copy with the scalar reference below and compare. */
	if (check)
		memcpy(g_HmrdpDwtRef, buffer, sizeof(g_HmrdpDwtRef));

	rfx_dwt_2d_decode_block(&buffer[3840], dwt_buffer, 8);
	rfx_dwt_2d_decode_block(&buffer[3072], dwt_buffer, 16);
	rfx_dwt_2d_decode_block(&buffer[0], dwt_buffer, 32);

	if (check)
		hmrdp_dwt_check(buffer);
}
'@
Patch-Regex $dwtC '(?<=#include "rfx_dwt\.h"\n\n).*?\nvoid rfx_dwt_2d_decode\(INT16\* WINPR_RESTRICT buffer, INT16\* WINPR_RESTRICT dwt_buffer\)\n\{.*?\n\}\n' $dwtNew 'HmrdpDwtReference'

