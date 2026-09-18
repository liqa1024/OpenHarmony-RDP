# 25) HmRdp: do not create the WinPR pool when the platform queue is the decode
#     executor.
#
#     背景（doc_agent/cpu-accel-plan.md §0）：progressive tile 解码统一由 ffrt 并发
#     队列执行（hmrdp_parallel.*，step 21），FreeRDP 自己的 WinPR 池在这条链路上已
#     无消费者。若仍按 `UseThreads` 建池，池会在每个 codec 上下文里常驻一组最小线程
#     （按核数），纯粹浪费。这一步在平台执行器可用时把 `UseThreads` 置 FALSE，于是
#     rfx_context_new_ex 不建池、不 InitializeThreadpoolEnvironment、不分配
#     workObjects，rfx_context_free 的对应释放分支也不进。
#
#     范围：`UseThreads` 只影响 rfx.c 自己的旧 RemoteFX 解码线程化路径（本工程只用
#     Progressive，不走那条）；progressive.c 的串行/并行判据已改为读
#     `HmrdpDecodeWidth()`（step 11），与此无关。弱符号：没有平台执行器的构建照旧
#     建池。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$rfxC = "$Source\libfreerdp\codec\rfx.c"

# (a) the platform-executor probe, at file scope (the call site is inside
#     rfx_context_new_ex).
Patch-Regex $rfxC '#define TAG FREERDP_TAG\("codec"\)\n' (@'
#define TAG FREERDP_TAG("codec")

/* HmRdp: the app's platform task queue (hmrdp_parallel.*). Weak, so a build
 * without it keeps the codec's own WinPR pool. */
extern int HmrdpParallelAvailable(void) __attribute__((weak));

static INLINE BOOL hmrdp_platform_executor_available(void)
{
	return (HmrdpParallelAvailable != NULL) && (HmrdpParallelAvailable() != 0);
}
'@ + "`n") 'hmrdp_platform_executor_available'

# (b) skip pool creation when the platform queue will run the tiles.
Patch-Regex $rfxC `
  '\tif \(priv->UseThreads\)\n\t\{\n\t\t/\* Call primitives_get here in order to avoid race conditions when using primitives_get \*/' (@'
	/* HmRdp: the progressive tile decode runs on the platform task queue
	 * (hmrdp_parallel.*), so the codec's own WinPR pool has no consumer left.
	 * Creating it would park a minimum-size worker set per codec context for
	 * nothing. */
	if (hmrdp_platform_executor_available())
		priv->UseThreads = FALSE;

	if (priv->UseThreads)
	{
		/* Call primitives_get here in order to avoid race conditions when using primitives_get */
'@) 'would park a minimum-size worker set'
