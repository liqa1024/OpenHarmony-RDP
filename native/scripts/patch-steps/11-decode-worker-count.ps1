# 11) HmRdp: the decode width comes from the app; the serial/parallel choice
#     follows it.
#
#     背景：多核执行器已统一到平台的 ffrt 并发队列（hmrdp_parallel.*，见 step 21）。
#     并行宽度是 app 的一个进程级值，app 通过 `HmrdpDecodeWidth()` 暴露；解码器只
#     需要据此在"接收线程串行"与"提交平台队列"之间选择。这里不再有任何 WinPR 池
#     相关接线（宽度不再是"池的 worker 数"）。
#
#     (a) 解码器读 app 导出的 `HmrdpDecodeWidth()`（弱符号；没有平台执行器的构建
#         返回 1 ⇒ 串行）。
#     (b) 串行分支的判据是宽度 <= 1（rfx 的 UseThreads 属于旧的池开关，不再参与）。
#
#     整块按"一次性整体打补丁"设计：改动它要从干净源码重打。
$progC = "$Source\libfreerdp\codec\progressive.c"

# (a) the app-provided width, next to the tile-chunk helpers.
Patch-Regex $progC 'static INLINE SSIZE_T progressive_process_tiles\(' (@'
/* HmRdp: the decode width requested by the app (hmrdp_parallel.*). Weak, so a
 * build without the platform executor decodes on the receiving thread. */
extern unsigned int HmrdpDecodeWidth(void) __attribute__((weak));

static INLINE UINT32 hmrdp_decode_width(void)
{
	return HmrdpDecodeWidth != NULL ? HmrdpDecodeWidth() : 1u;
}

static INLINE SSIZE_T progressive_process_tiles(
'@) 'hmrdp_decode_width'

# (b) the serial branch: width 1 (or no platform executor) decodes on the
#     receiving thread, with no task submission.
Patch-Regex $progC '\tif \(!progressive->rfx_context->priv->UseThreads\)\n\t\{\n\t\t/\* Serial: one call per tile, exactly as before the chunking change\. \*/\n\t\tfor \(UINT32 idx = 0; idx < region->numTiles; idx\+\+\)\n\t\t\tprogressive_process_tiles_tile_work_callback\(0, &progressive->params\[idx\], 0\);\n\n\t\tgoto fail;\n\t\}\n' (@'
	if (hmrdp_decode_width() <= 1)
	{
		/* Serial (width 1, or no platform executor): one call per tile, no task
		 * submission. */
		for (UINT32 idx = 0; idx < region->numTiles; idx++)
			progressive_process_tiles_tile_work_callback(0, &progressive->params[idx], 0);

		goto fail;
	}
'@ + "`n") 'no task submission'
