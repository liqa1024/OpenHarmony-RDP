# 26) HmRdp: let a dev probe route width 1 through the platform queue.
#
#     背景（doc_agent/cpu-accel-plan.md §5）：宽度 1 的解码在接收线程内联执行
#     （step 11 的串行分支），既不提交、也不等待，所以并行段账目（`par`）看不到它。
#     要把"宽度 1 走队列"和"宽度 1 串行"对照起来，需要让串行分支**可被放行**：
#     队列里跑一个 task（并发上限 1），从而量出执行器自身的代价（提交 + 唤醒 +
#     等待）。
#
#     绑定方式沿用弱符号约定：`HmrdpParallelForceQueue()` 由 app
#     （hmrdp_parallel.*）提供，未导出的构建返回 0 ⇒ 保留原串行分支。
#     这是 dev A/B，不是正常运行路径（正常运行时 app 返回 0）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progForceC = "$Source\libfreerdp\codec\progressive.c"

# (a) the app-provided switch + helper, next to the tile-home hook.
Patch-Regex $progForceC `
  'extern int HmrdpTileHomeMode\(void\) __attribute__\(\(weak\)\);\n\nstatic INLINE SSIZE_T progressive_process_tiles\(' (@'
extern int HmrdpTileHomeMode(void) __attribute__((weak));

/* HmRdp dev: non-zero when the app wants width 1 to run on the platform queue
 * (one task on a concurrency-1 queue) instead of the receiving thread's serial
 * loop. It exists to measure the executor's own cost against the serial branch;
 * a build without it, or an app that returns 0, keeps the serial branch. */
extern int HmrdpParallelForceQueue(void) __attribute__((weak));

static INLINE BOOL hmrdp_force_queue(void)
{
	return (HmrdpParallelForceQueue != NULL) ? (HmrdpParallelForceQueue() != 0) : FALSE;
}

static INLINE SSIZE_T progressive_process_tiles(
'@) 'HmrdpParallelForceQueue'

# (b) the serial branch yields when the probe asks for the queue.
Patch-Regex $progForceC `
  '\tif \(hmrdp_decode_width\(\) <= 1\)\n\t\{\n' (@'
	/* HmRdp dev: width 1 decodes on the receiving thread unless the
	 * executor-cost probe asks for the queue (see HmrdpParallelForceQueue). */
	if ((hmrdp_decode_width() <= 1) && !hmrdp_force_queue())
	{
'@) 'hmrdp_force_queue()'
