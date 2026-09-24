# 29) HmRdp: drop the upstream NSC NEON "TODO: Implement ..." warning
#     (doc_agent/native-libraries.md §3). The stub only corresponds to the
#     encode side and a client decodes only, so it stays an empty no-op.
$nscNeonC = "$Source\libfreerdp\codec\neon\nsc_neon.c"

$nscNeonOld = @'
	if (!IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
		return;

	WLog_WARN(TAG, "TODO: Implement neon optimized version of this function");
'@
$nscNeonNew = @'
	/* HmRdp: nothing to wire in here. The upstream NEON file only ever carried
	 * the encode-side optimization (cf. codec/sse/nsc_sse2.c) and a client only
	 * decodes, so the stub stays empty - without the upstream TODO warning. */
	WINPR_UNUSED(context);
'@
Patch-Block $nscNeonC $nscNeonOld $nscNeonNew 'HmRdp: nothing to wire in here'
