# RDP 剪贴板与文件传输

本文件是 **RDP 侧剪贴板通道**的家：cliprdr 格式协商与线格式、文件传输与实现坑。两协议共享的剪贴板机制
（手动语义、共享控制器 / 桥 / 本机读写叶子、文案）见 [`clipboard.md`](clipboard.md)；原生边界判据见
[`architecture.md`](architecture.md) §3.1。

## 1. 格式协商与原生 / ArkTS 分工

原生覆写 cliprdr 的 `Server*` 回调与 `MonitorReady`，只从 `Client*` 读/发它需要的表示。
当前支持 **纯文本 / 富文本（HTML）/ 图片 / 文件**；优先级 **文件 > 图片 > HTML > RTF > 文本**。
两个按钮按缓存到的最丰富类型给差异化提示。

**接收端的图片有两种 wire 形式**：DIB/DIBV5 与注册格式 `PNG`。原生 `OnCliprdrServerFormatList` 按格式名认这两种，
候选里 DIB 排在 PNG 之前（图片层内 **DIB 优先、PNG 兜底**）；客户端只提供 PNG 时也能「复制」到图片——
PNG 走新事件 `kClipboardImagePng`，ArkTS 用共享 `DECODE_PNG` 解码（与 Moonlight 同一条图片路径）。

**原生只搬字节，"选哪个"在 ArkTS**（[`architecture.md`](architecture.md) §3.1）：`FORMAT_LIST` 到达时原生
按 wire 形式把受支持格式分类成候选（`kind:id`，同一 kind 只留一个，DIB 先于 DIBV5）并发 `RemoteFormatList`
事件；**优先级、单在途、延迟刷新都在 ArkTS**（`SessionPage.onRemoteFormatList` 按优先级选一种，经
`requestRemoteFormat` 下发）。数据响应不带格式 id ⇒ 同一时刻**只允许一个请求在途**；原生记下在途请求的
kind 并在响应上回带，另发一个 `RemoteFormatResponse`（无论成功/空/失败）作为"本次已结束"的信号，ArkTS
据此取它在这期间记下的最新待刷新项。`FORMAT_DATA_REQUEST`（服务端拉本机内容）是**同步 serve**，仍在原生。

## 2. 文件传输

- 入口沿用两个按钮（不新增独立按钮）：远端剪贴板是文件列表时「复制」触发下载，本机剪贴板含文件 URI
  时「粘贴」触发上传。仍**手动触发**，不做自动或监听。
- **协议**：先传 `FileGroupDescriptorW`（u32 数量 + 592 字节 `FILEDESCRIPTORW` 列表），再按
  `lindex`/`streamId` 用 `CB_FILECONTENTS_REQUEST/RESPONSE` 流式传字节（`FILECONTENTS_SIZE` 取
  64 位大小、`FILECONTENTS_RANGE` 取块；描述符已带 `FD_FILESIZE` 时跳过 SIZE，与 Windows 客户端一致）。
  客户端能力位取 `CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED`
  ——**不要置 `CB_CAN_LOCK_CLIPDATA`**：一旦协商了锁，服务端会下发 `clipDataId` 并要求**每个**文件内容请求
  回带，未回带的请求被 `CB_RESPONSE_FAIL` 拒绝，表现为「远程拒绝提供文件内容」、上传方向同样卡住；
  FreeRDP 自带的客户端文件传输也把这个位置注释掉（`client_cliprdr_file.c` 的
  `cliprdr_file_context_current_flags`）。通道把客户端声明的标志与服务端能力**取交**（`cliprdr_main.c`），
  服务端不支持时自动降级。文件格式 id 在客户端注册范围自选（`0xC001`/`0xC002`），服务端按**格式名**识别
  （依赖已协商的 `CB_USE_LONG_FORMAT_NAMES`）。服务端下发的能力标志在 `HmrdpCliprdrServerCapabilities` 里记
  hilog，用来核对取交结果。
- **字节的源/汇由 App 提供**：FreeRDP 的 client cliprdr 只实现线协议（分块、stream 状态、huge 校验、
  lock 转发），内容要 App 自己读写。分工按 [`architecture.md`](architecture.md) §3.1 的判据：**上传**是
  server 拉块、回调须在 RDP 线程**同步应答**，故留在原生（按请求从沙箱文件回块）；**下载**不是同步回调
  （只要求在回调期间把字节拷走），故状态机整个在 ArkTS——原生把每个 `FILECONTENTS_RESPONSE` 拷成
  `RemoteFileChunk` 事件交给 ArkTS，由它写沙箱文件、维护 offset/size 序列并发下一个请求
  （`requestRemoteFileContents`，一次一个在途）。文件块用独立的 `streamId` 序列。
- **平台侧**：本机→远端用系统安全控件授权 + `getDataWithProgress({destUri})`，由系统把剪贴板文件拷进
  应用沙箱（无需 `READ_PASTEBOARD`，也不必手工处理 URI 授权）；远端→本机先落沙箱，完成后用
  `fileUri.getUriFromPath` + `MIMETYPE_TEXT_URI` 写回本机剪贴板，其他应用粘贴时由框架按 URI 取。
  `getDataWithProgress` **不支持文件夹**，故不递归目录；非文件 URI（网页链接）走文本路径。
- **落地与失败**：下载目录按时间戳分桶避免覆盖；描述符未带 `FD_FILESIZE` 时才逐文件发 `FILECONTENTS_SIZE`；
  任一文件失败即中止并清掉本次分片。**上传**进度由原生随同步 serve 上报（`kFileTransferProgress`，载荷
  `方向|已完成|总量|第几个|共几个`）；**下载**进度在 ArkTS 本地按自身 done/total 计算，不跨边界。
  速率均由 UI 按相邻两次 `done` 差值算，不进原生。工具栏内联显示「第几个/共几个 + 百分比 + 速率 +
  较宽的进度条 + 取消」——多文件时靠计数看出整体在推进，不单看进度条。
  取消分两种：
  **上传未开始拉取**（`localTransferDone_ == 0`）时是**干净撤回**——把本地剪贴板改广告成空文本
  （`CF_UNICODETEXT` + 2 字节 NUL），服务端据此丢掉文件格式，之后远端粘贴得到空文本而不是报错；
  **上传已开始拉取**时只能停服，远端粘贴会报协议错误，故这种情况先弹一次确认。**下载**在 ArkTS 侧
  直接停并清掉本次分片。下载完成由 ArkTS 写本机剪贴板、上传完成走 `kFileTransferDone`。
- **上传完成后必须撤回文件剪贴板**（`WithdrawLocalFileClipboard()`，即上面「未开始拉取」那一段逻辑）：
  广告出去的列表字节全部送完之后（`localTransferDone_ >= localTransferTotal_`，全零字节的文件在描述符响应里
  即算完成），远端若再粘贴一次会**重新拉同一批文件**，而此时 UI 的进度行与取消已随完成清空，表现成「没有进度条
  的重传」。故两处完成点在发 `kFileTransferDone` 之后立刻换成空文本广告——字节已取完，换内容不会打断本次拉取，
  服务端据此丢掉文件格式，之后再粘贴什么也拉不到。上传被取消时走同一段代码，不许分叉出两套清理。
- **上传是"广告"而非推送，不能预传**：cliprdr 的文件字节是**服务端拉取**模型，只有远端粘贴时服务端才发
  `FILECONTENTS_REQUEST`，客户端无法主动推，所以点「粘贴」后进度停在 0 直到对端粘贴。同理想"撤回"也
  只能靠换内容——空 `FORMAT_LIST` 会被通道直接丢弃（`cliprdr_main.c`：首次之后 `numFormats == 0`
  忽略）。故上传方向：点按钮后**在进度行原位**显示「在远端粘贴开始传输」提示（带转圈与取消），
  **拉取真正开始（`done > 0`）才换成进度行与取消**；不用 toast 提示（字数多且一闪而过看不清）。
- **不支持文件夹（两向）**：文件剪贴板只流文件。远端列表里出现任一目录即**整份拒绝**（不做部分下载）；
  本机侧选到文件夹同样拒绝。提示统一为「不支持文件夹传输」。混合列表情形因此不存在（有目录即拒绝）。
- **传输期间工具栏不自动隐藏**：只要有传输在跑，或上传还停在「在远端粘贴开始传输」，全屏工具栏就被
  **钉住显示**（不按悬停/延迟收起），让进度和提示始终可见；传输结束（成功/失败/取消）恢复正常的悬停显隐。
- **沙箱临时文件的清理口径**：每次传输新建 `<时间戳>` 目录，而剪贴板只引用**最新**那一个，所以新建时删除同
  root 下的旧目录（`rdp_inbox` / `rdp_outbox` 各自只保留一个），占用因此恒定，也不会破坏当前剪贴板里仍然
  有效的 URI；取消/失败时删掉本次已落盘的分片（下载侧在 ArkTS）。**不在启动或会话结束时清**——前者会让上个
  进程残留在剪贴板里的 URI 失效，后者在多窗口下会误删别的窗口刚写入的目录。
- **一次只允许一个文件传输**：两个按钮在 `transferBusy || transferActive || transferWaiting` 时直接拒绝
  并提示「正在传输文件，请先取消或等待完成」。否则两条状态机会挤在同一个进度行里，上传侧更糟——服务端正在
  按 `listIndex` 拉取时替换文件列表会让索引错位。**中止必通知**：远端 `FORMAT_LIST` 变化时原生发
  `kRemoteClipboardChanged`，ArkTS 据此中止自己正在跑的下载（清理分片并收起进度行），避免进度行等一个
  永远不会来的事件；点击复制时列表已空则直接提示。

## 3. 实现坑（改剪贴板前必看）

- **大图剪贴板冻结画面（服务端侧，客户端不是瓶颈）**：把图片推到远端时若只广告未压缩 `CF_DIB`（整屏
  32bpp 可达数十 MB），**服务端**在接收/消化它期间停止发画面更新，表现为画面与输入冻结好几秒。客户端
  把该载荷交给通道仅数毫秒、RDP 线程在整段冻结里无长阻塞 ⇒ 瓶颈在服务端、不在客户端发送。**解法是
  减小载荷**：同时广告压缩的注册格式 `PNG`（置于 `CF_DIB` 之前，服务端会拉 PNG），载荷通常小一个量级，
  冻结随之消失；`CF_DIB` 保留为服务端不支持 PNG 时的兜底。文件不受影响：字节走 `FILECONTENTS` 分块。
- **远端格式协商**：`FORMAT_LIST` 的候选分类在原生（wire 形式），`文件(FileGroupDescriptorW) > 图片(DIB/DIBV5)
  > HTML Format > Rich Text Format > CF_UNICODETEXT` 的**优先级选择在 ArkTS**（见 §1）。**数据响应不带
  格式 id** ⇒ 同一时刻**只允许一个请求在途**，期间的新列表记为待刷新，`RemoteFormatResponse` 回来再取；
  分派时**严格按请求时的 kind**，绝不用二进制兜底当文本（DIB 头 `28 00 00 00` 按 UTF-16 会变成 `(`）。
- **本机 HTML 读取**：不能只看 `getPrimaryHtml()`（只读第一条记录，且富文本常把 `text/plain` 作主 MIME、
  HTML 作附加 Entry）。要**遍历记录**用 `record.getData('text/html')` 取，再回退文本；
  不要用 `getMimeTypes()` 做门控（它可能只列主类型）。
- **HTML 只加壳、不改内容**：若源 HTML 已自带 `<!--StartFragment-->`/`<!--EndFragment-->`（Word/WPS
  导出都带）就**复用**，只算字节偏移（CF_HTML 偏移是**字节**，不是字符），不注入重复标记、不规范化
  原内容。写本机时若远端只给片段，用最小 `<html><body>` 包成良构文档；多格式必须落在**同一 Record
  的不同 Entry**，不能建两条 Record。
- **RTF 兜底**：很多 Windows 应用只显式提供 `Rich Text Format` 而无 `HTML Format`（HTML 是 Word 在 OLE
  层按需合成的，`EnumClipboardFormats` 枚举**不会触发**合成），故客户端自带 RTF→HTML 转换。
  注意 `\colortbl` 索引约定：第一个 `;` 是索引 0（auto），之后每个 `;` 递增，解析时**不要预置空条目**，
  否则 `\cfN` 整体错位、颜色丢失。
