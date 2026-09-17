# 17) HmRdp: the NEON inverse DWT is no longer bit-exact-with-the-reference-only
#     territory (doc_agent/gfx-engine.md §8.4): what matters is that the
#     difference is a *rounding* difference and imperceptible, so the NEON
#     variants are allowed in as long as that is measured rather than assumed.
#
#     Two things are needed for that:
#       (a) the dev comparison has to run wherever the decode happened, i.e. also
#           inside the NEON entry point (with -DWITH_SIMD=ON that is the one that
#           runs; the C entry in progressive.c never gets called). Its reference
#           is HmrdpDwtExtrapolateReference() from step 16, so the number it
#           reports is "NEON vs the upstream scalar arithmetic", and the worst
#           |delta| it reports is what tells a rounding difference (a few LSB)
#           from a wrap-around (thousands).
#       (b) the *non-extrapolated* NEON DWT stays switched off: it does the same
#           `vaddq_s16` on two neighbours, so it wraps for large coefficients -
#           not a rounding difference - and this variant never runs on
#           Progressive content, i.e. there is no measurement to weigh it with.
#           The bit-exact implementation from step 15 stays in that slot.
$neonC = "$Source\libfreerdp\codec\neon\rfx_neon.c"
$neonExterns = @'
#include <winpr/sysinfo.h>

/* HmRdp dev: the inverse-DWT comparison against the bit-exact reference. Both
 * live in this library (patch-freerdp.ps1 steps 15/16). */
extern int HmrdpDwtCheckArmed(void);
extern void HmrdpDwtCheckResult(unsigned int bad, int maxDelta);
extern void HmrdpDwtExtrapolateReference(INT16* buffer, INT16* temp);
'@
Patch-Block $neonC '#include <winpr/sysinfo.h>' $neonExterns 'HmrdpDwtExtrapolateReference'

$neonEntryOld = @'
static void rfx_dwt_2d_extrapolate_decode_neon(INT16* buffer, INT16* temp)
{
	WINPR_ASSERT(buffer);
	WINPR_ASSERT(temp);
	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[3807], temp, 3);
	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[3007], temp, 2);
	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[0], temp, 1);
}
'@
$neonEntryNew = @'
static void rfx_dwt_2d_extrapolate_decode_neon(INT16* buffer, INT16* temp)
{
	static INT16 ref[4096] = { 0 };
	static INT16 scratch[4096] = { 0 };
	const BOOL check = HmrdpDwtCheckArmed();

	WINPR_ASSERT(buffer);
	WINPR_ASSERT(temp);

	/* HmRdp dev: the decode overwrites the whole coefficient buffer, so the input
	 * is copied first; the reference then decodes that copy below, and both the
	 * number of different elements and the worst |delta| are reported. */
	if (check)
		memcpy(ref, buffer, sizeof(ref));

	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[3807], temp, 3);
	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[3007], temp, 2);
	rfx_dwt_2d_decode_extrapolate_block_neon(&buffer[0], temp, 1);

	if (check)
	{
		UINT32 bad = 0;
		int maxDelta = 0;
		size_t i = 0;

		HmrdpDwtExtrapolateReference(ref, scratch);

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
Patch-Block $neonC $neonEntryOld $neonEntryNew 'the reference then decodes that copy below'

$neonInitOld = @'
		context->quantization_decode = rfx_quantization_decode_NEON;
		context->dwt_2d_decode = rfx_dwt_2d_decode_NEON;
		context->dwt_2d_extrapolate_decode = rfx_dwt_2d_extrapolate_decode_neon;
'@
$neonInitNew = @'
		context->quantization_decode = rfx_quantization_decode_NEON;
		/* HmRdp: the non-extrapolated DWT keeps the bit-exact implementation
		 * (codec/rfx_dwt.c, step 15). The NEON one above adds two int16
		 * neighbours in 16-bit lanes and wraps when they do not fit, which is
		 * not a rounding difference; and this variant never runs on Progressive
		 * content, so there is no measurement to weigh it with. */
		WINPR_UNUSED(rfx_dwt_2d_decode_NEON);
		context->dwt_2d_extrapolate_decode = rfx_dwt_2d_extrapolate_decode_neon;
'@
Patch-Block $neonC $neonInitOld $neonInitNew 'the non-extrapolated DWT keeps the bit-exact implementation'

