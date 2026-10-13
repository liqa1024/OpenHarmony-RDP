# 25) Do not create the codec's WinPR pool when the platform queue is the decode executor
#     (UseThreads=FALSE); progressive.c's serial/parallel choice is independent (step 11).
$rfxC = "$Source\libfreerdp\codec\rfx.c"

Patch-Regex $rfxC '#define TAG FREERDP_TAG\("codec"\)\n' (@'
#define TAG FREERDP_TAG("codec")

/* HmRdp: the app's platform task queue (hmrdp_parallel.*); weak, so a build without it keeps the WinPR pool. */
extern int HmrdpParallelAvailable(void) __attribute__((weak));

static INLINE BOOL hmrdp_platform_executor_available(void)
{
	return (HmrdpParallelAvailable != NULL) && (HmrdpParallelAvailable() != 0);
}
'@ + "`n") 'hmrdp_platform_executor_available'

Patch-Regex $rfxC `
  '\tif \(priv->UseThreads\)\n\t\{\n\t\t/\* Call primitives_get here in order to avoid race conditions when using primitives_get \*/' (@'
	/* HmRdp: the platform task queue runs the tiles, so creating the WinPR pool would park a minimum-size worker set for nothing. */
	if (hmrdp_platform_executor_available())
		priv->UseThreads = FALSE;

	if (priv->UseThreads)
	{
		/* Call primitives_get here in order to avoid race conditions when using primitives_get */
'@) 'would park a minimum-size worker set'
