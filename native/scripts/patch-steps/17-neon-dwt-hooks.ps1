# 17) HmRdp: the NEON inverse DWT hooks (doc_agent/native-libraries.md §3).
#
#     The extrapolated (reduced) DWT is the one a Progressive region actually
#     runs, so -DWITH_SIMD=ON wires the NEON entry point into it; its arithmetic
#     is the upstream NEON one and the difference it makes is a rounding-level
#     one (measured when it went in, doc_agent/gfx-engine.md §8.2).
#
#     The *non-extrapolated* NEON DWT stays switched off: it does the same
#     `vaddq_s16` on two neighbours, so it wraps for large coefficients - not a
#     rounding difference - and this variant never runs on Progressive content,
#     i.e. there is no measurement to weigh it with. The bit-exact implementation
#     from step 15 stays in that slot.
$neonC = "$Source\libfreerdp\codec\neon\rfx_neon.c"

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
