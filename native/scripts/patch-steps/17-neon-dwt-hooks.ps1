# 17) Wire the NEON extrapolated inverse DWT entry point in (WITH_SIMD=ON); the non-extrapolated
#     NEON DWT stays off (wraps on large coefficients, never runs on Progressive content).
$neonC = "$Source\libfreerdp\codec\neon\rfx_neon.c"

$neonInitOld = @'
		context->quantization_decode = rfx_quantization_decode_NEON;
		context->dwt_2d_decode = rfx_dwt_2d_decode_NEON;
		context->dwt_2d_extrapolate_decode = rfx_dwt_2d_extrapolate_decode_neon;
'@
$neonInitNew = @'
		context->quantization_decode = rfx_quantization_decode_NEON;
		/* HmRdp: the non-extrapolated DWT keeps the bit-exact implementation (codec/rfx_dwt.c,
		 * step 15); the NEON one wraps on large coefficients and never runs on Progressive content. */
		WINPR_UNUSED(rfx_dwt_2d_decode_NEON);
		context->dwt_2d_extrapolate_decode = rfx_dwt_2d_extrapolate_decode_neon;
'@
Patch-Block $neonC $neonInitOld $neonInitNew 'the non-extrapolated DWT keeps the bit-exact implementation'
