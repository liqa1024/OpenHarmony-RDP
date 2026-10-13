# 29) Drop the upstream NSC NEON "TODO: Implement ..." warning: the stub is encode-side only
#     and the client decodes only, so it stays an empty no-op.
$nscNeonC = "$Source\libfreerdp\codec\neon\nsc_neon.c"

$nscNeonOld = @'
	if (!IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
		return;

	WLog_WARN(TAG, "TODO: Implement neon optimized version of this function");
'@
$nscNeonNew = @'
	/* HmRdp: nothing to wire in here; the NEON stub is encode-side only and the client only decodes. */
	WINPR_UNUSED(context);
'@
Patch-Block $nscNeonC $nscNeonOld $nscNeonNew 'HmRdp: nothing to wire in here'
