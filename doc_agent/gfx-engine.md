# GFX 码流 / GPU 桌面引擎

本文件是**改 GFX 相关代码前必读**的口径文档。目标：引擎与 FreeRDP 自己的 gdi 软解**逐像素一致**
（验收见 §6）。

## 0. 管线框架（先读这一节）

### 0.1 通道与两层压缩

- 一条 RDP 连接复用多条通道（GFX、输入、剪贴板、音频…），**视觉管线只属于 GFX 通道**
  （动态虚拟通道 `Microsoft::Windows::RDS::Graphics`）；上行（键鼠输入、能力协商、帧回执、缓存导入）
  **不产生画面**。
- GFX 通道里有**两层压缩**：
  1. **传输级 ZGFX**：整条字节流批量压缩，分段且**可原样透传**（压不动就发原样段）。对**已熵编码的图像
     载荷**（Progressive / ClearCodec…）基本压不动（客户端解压≈拷贝），只有未压缩位图/元数据才真正被压缩；
  2. **图像码流**：命令载荷自身的编码（Progressive / ClearCodec / …），这才是真正的"图像解压"。
- 客户端接收链：**ZGFX 解压 → RDPGFX PDU 解析** → 「操作命令 + **仍压缩的**图像载荷」。
- **分工结论**：ZGFX + PDU 解析留在 CPU（廉价，且产物是必须串行处理的"控制+图像"混合流）；
  **图像载荷不解压，直接送 GPU 引擎**。

### 0.2 从命令到表面像素

- 客户端为每个 surface 持有**持久缓冲**；服务端用 `surfaceId` 寻址，只下发"对该表面某区域的操作"。
- 图像命令 = 一条码流载荷 + 一个**目标区域**（矩形；渐进式是"分块 + 有效区域"）。
- **顺序是语义的一部分**：严格按命令到达顺序执行；重叠时"后覆盖先"；表面拷贝、缓存存取、渐进细化都
  依赖**当前表面内容**。
- **一致性前提**：客户端表面内容必须与服务端认为的一致——后续命令会读它，且解错**不会自动纠正**
  （除非该区域之后被重新覆盖）。这就是"逐像素对齐"是硬要求的原因。
- 像素操作只有四类：**解码写入 / 纯色填充 / 表面拷贝 / 缓存存取**；最终都是"把某矩形内的像素写成确定值"，
  写进**同一张表面图**，多张表面再合成到**可见桌面图**。

### 0.3 各码流（本工程范围）

| codecId | 名称 | 解码 | 写入方式 | 读旧像素 | 跨命令状态 | 说明 |
|---|---|---|---|---|---|---|
| 0x0000 | Uncompressed | 无（直接拷贝） | 目标矩形整块覆写 | 否 | 无 | 24/32bpp |
| 0x0008 | **ClearCodec** | 有（`clear_decompress`） | 目标矩形，**可能只覆盖部分像素** | **是** | **会话级**（序列号 + 字形/竖条缓存） | §2.2 |
| 0x0009 | CAPROGRESSIVE | 有 | 64×64 分块 + 有效区域、多次细化 | 区域外保留 | **每块持久状态** | 本工程主用 |
| 0x000A | Planar | 未实现 | — | — | — | 检测到即回退 |
| 0x000D | CAPROGRESSIVE_V2 | **未实现**（FreeRDP 亦未实现） | — | — | — | 检测到即不支持 |

- **NSC 不是独立 GFX 码流**：它是 ClearCodec 内部的一种子编码，别与 Planar 混淆。
- **渐进分块的顺序**：同一条消息内的分块**互不重叠、可任意顺序**（可并行）；**跨消息的同一分块必须按序**
  （先粗后细在持久状态上累积）；与其他命令之间仍是全局按序。

### 0.4 从表面到屏幕（合成 / 脏区 / 上屏）

1. 只取**映射到输出**的表面（未映射表面不参与上屏）；
2. 取该表面的**脏区**（命令执行时标记的矩形并集）∩ 映射矩形；
3. **合成到主画面**：**1:1 拷贝** + 格式转换 + 裁剪到桌面边界；多表面按顺序叠加（协议未定义输出表面间的
   z 序，实践上不重叠）。服务器端缩放映射不支持；
4. 把写入的矩形并入**主画面脏区**；清空该表面脏区；
5. **上屏门控**：主画面脏区**为空 → 不上屏**（静止，或只收到离屏/缓存类更新）→ **FPS = 0**；
   非空 → **只上传脏矩形**；
6. 上屏完成后服务端等**帧回执**（通道层在 `EndFrame` 返回后发送）。

真正的"后处理"只有：**缩放（可选）+ 格式转换 + 裁剪 + 脏区合并**；没有滤波/锐化/去块，也没有跨表面
alpha 混合。远端光标独立处理，不混进主画面缓冲。

### 0.5 "静止 / 部分变动 / 大量变动"

这不是客户端的处理分支，而是**输入形态**（由**服务端**在发送前选择命令/码流决定）：静止=不发绘制命令
（最多管理/回执）→ 无脏区 → 不上屏；部分变动=少量命令/小矩形/缓存拷贝 → 小脏区；大量变动=多条命令/
整屏块 → 大脏区。客户端走**同一条流水线**，显式分支只有：① 命令分类；② 帧内/帧外；③ 是否映射到输出；
④ 主画面脏区是否为空。

---

## 1. 码流分工

- **Progressive / 未压缩位图 / 表面绘制 → GPU**（SPIR-V compute + transfer），引擎直写表面缓冲；
  Progressive 的解码拆成**两个 kernel**（`rfx_decode.comp` + `rfx_idwt.comp`）：
  1. `rfx_decode`：一条 lane 一条 (tile,component) stream，跑 RLGR/去量化/差分与持久状态同步；
  2. `rfx_idwt`：**一个 workgroup 一条 stream**，把该 stream 的 4096 个系数搬进 **shared memory**
     再跑三级逆 DWT，全局只读一次写一次。
     **为什么必须拆**：这个变换原本在全局 SSBO 上做多趟 16bit 读-改-写，是当时 decode 的最大头；
     搬进 shared 后 decode 的 GPU 时间显著下降。语义（子带偏移/长度、每步 INT16 截断、差分顺序、
     位状态）与 §2.1 逐条一致，`Vulkan对比` 仍是 `bad=0` 的门禁。
- **`rfx_compose` 必须按像素并行**（一个 lane 一个像素）：按 tile 并行时 32 个 lane 会写 32 个不同 tile
  （每 4 个有效字节占一条 cache line），且每像素还要遍历整条裁剪 rect 列表；改成按像素 + 由 host 把
  裁剪 rect 预先算成**tile 内局部坐标**存进 tileMeta 后，该 dispatch 快了约两个数量级。
- **ClearCodec 留在 CPU**：它不是自包含的（未覆盖像素保留原值），复用 FreeRDP 的 `clear_decompress`
  对**持久映射的表面缓冲**做读改写（共享内存，**不搬 GPU、不做逐区域跨侧往返**）；
- **表面/缓存存储**是**持久映射的 host-visible 线性缓冲**（屏幕仍是 image），于是 CPU 访问零成本，
  不需要 staging / 回读 / 布局状态机；
- **host-visible 类型必须选 `HOST_CACHED`**（若设备提供）：`DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`
  是 uncached 的，CPU 读只有 **~120 MB/s**（实测：本仓库引擎的内存探针），而引擎所有 CPU 像素命令
  （ClearCodec、cache 存取、`SurfaceToSurface`、填充、未压缩上传）都是"读+写映射"⇒ 会直接变成瓶颈；
  同一设备上有 `HOST_CACHED` 类型时跨行读 ~11 GB/s（快约 20~180 倍）。
  **约束**：cached 类型不是 `HOST_COHERENT`，所以 CPU/GPU 交接必须显式做**范围级缓存维护**——
  CPU 写过的映射在 submit 前 `vkFlushMappedMemoryRanges`，fence 等待后、CPU 读之前
  `vkInvalidateMappedMemoryRanges`（按行切 range、对齐 `nonCoherentAtomSize`）；
  CPU 触碰一片 rect 之前也要先 invalidate 该 rect（部分行写入不能把陈旧邻居写回）。
  CPU-only 的缓冲（bitmap cache 项）只被 CPU 访问，不需要任何维护。
- **CPU 写过的表面被 GPU 读取前必须补 `HOST → TRANSFER` barrier**（UMA 不等于免费）。

开关：全局「硬件解码」。**注意当前没有引擎接进 live 会话**（Vulkan 引擎只跑回放/对比），
所以 live 一律走 gdi；将来 Vulkan 接管时，口径是 关 / 无 Vulkan / 引擎初始化失败 ⇒ 回退 **gdi**（见 §7）。

> **「真机专属功能」**：GPU 引擎与 GPU 回放只在真机上验证与使用；**模拟器不参与**（其 Vulkan 实现会按
> 标准接口谎报能力）。模拟器上不要开硬件加速、不要跑 GPU 回放，也不要拿模拟器结论约束真机行为。

## 2. 必须保留的协议/算法语义（**与后端无关**，改引擎/着色器前逐条对照）

这些不是"实现细节"，而是与 FreeRDP 对齐的**行为契约**。少一条，引擎与 gdi 就会分叉。

### 2.1 解码层

- **RLGR 必须 64 位读位**（`RLGR1`）；零游程用 `1<<k` 累加，`kp` 上限 80、`>>3` 得 `k`。
- 去量化 `shift = quant + progQuant − 1`；`shift == 0` 或 `>= 16` **不改动**该子带
  （FreeRDP 的 `lShiftC_16s_inplace` 在这两种情况下直接返回）。
- `RFX_TILE_DIFFERENCE`（`flags & 1`）：与持久 `current` 做**饱和加法**，并把结果**同时写回** `current`。
- UPGRADE 的 SRL/raw **两个位流同时活跃**：非 LL 子带走 raw（按 `sign` 决定符号），LL3 走 SRL。
- 每 `(tile, 分量)` 的 `current` / `sign` / `bitPos` **跨消息常驻**；`bitPos` 每次解码都写为新的
  `quant + progQuant`。位状态缓冲**每流整字节对齐**（不要 10 字节紧排：相邻流会共享 32 位字、丢 RMW 更新）。
- **WBT 块状态机是"每条消息"的**：`progressive_decompress` 开头 `progressive->state = 0`，所以
  "REGION 在 FRAME_BEGIN 之前 / FRAME_END 之后" 这条忽略规则只看**本条消息**里的块（引擎原先把
  `frameBegin/frameEnd` 跨消息持久化，会接受 gdi 跳过的 region）。**被忽略的 region 连同它的 tile
  状态更新一起不生效**。
- **畸形消息的拒收粒度照抄 FreeRDP**：region 头/分量量化表校验失败 ⇒ 整个 region 什么都不做（两边
  一致）；但**"读了这么多 tile"之后才失败**（tile 头长度、块字节数、`numTiles` 不一致）时，FreeRDP
  已经把读到的 tile **登记进本帧的 tile 列表**（并更新其元数据），只是不进入解码/合成。UPGRADE 的
  `aSrlLen/aRawLen` 与声明长度不符时，FreeRDP 在**已把 refinement 累加进 `current`/`sign` 之后**返回
  失败 ⇒ 该 tile 的像素保持旧值但系数状态已变。
  - 引擎实现：UPGRADE 的这条拒绝判定由解码着色器逐流算出，写在**该流位状态条目的闲置字节**里
    （前 10 字节是各带 `bitPos`，`kBitPosStride=12`），合成时读它 ⇒ 任一分量被拒就**整块不合成**；
    判定在"kFirst 解码成功"时清除（type 3 重合成不清），于是旧像素一直保持到该 tile 重新解码成功。
    改位状态缓冲布局时必须同步这两处。
  - 判定的记账口径是**"请求过的位数"**（FreeRDP 的 `BitStream::position`：越界也照加，
    `BitStream_Shift` 对 ≥32 的位移不计数），不是"流里实际有多少位"。
- **UPGRADE 的 `numBits` 会 BYTE 回绕**：`numBits = tile->yBitPos - yBitPos` 在"本次比该 tile 上次更粗"
  （`ob < nb`）时回绕成 ~226~255；此时 FreeRDP 的 `BitStream_Shift` 对 ≥32 的位移**什么都不做**，而读出的
  值仍按硬件 5 位掩码取向 ⇒ **读到垃圾且不消耗位**，长度校验照样通过。把它钳成 0（=跳过该带）或真去
  消耗 n 位都会分叉。
- 逆 DWT 的抽取/尾块（`ProgIdwtX/Y`）与带偏移/长度必须照抄（抽取路径 0/1023/2046/3007/3279/3551/
  3807/3879/3951/4015；非抽取路径不同，不要混用）。
- 颜色转换：`yCbCrToRGB` 的 `(y+4096)<<16` + 乘系数后 `>>21`，系数是 **float 截断**得到的整数
  （`static_cast<int>(1.402525f * 65536)` 之类），不要用浮点现算。

### 2.2 合成层（**最容易出错、也最"看不见"的部分**）

- **compose 必须按桌面尺寸/region clip 裁剪**：桌面宽高非 64 倍数，边缘 tile 若不丢像素会按 stride
  折回下一行、污染邻接 tile。
- **`update_tiles` 的「同帧图块重复合成」必须实现**：FreeRDP 每收到一条 Progressive 消息，都会用
  **本消息的 clippingRects** 合成 **"本帧至今解码过的全部 tile"**（`surface->numUpdatedTiles` 只在
  **frameId 变化**时清零 ⇒ 即 RDPGFX StartFrame），而不只是本消息的 tile。
  - 引擎做法：每条消息为"本帧更早的 tile"补一遍合成（从持久 `current` 重跑逆 DWT——
    **不需要缓存 tile 像素**：逆 DWT 是确定性的，而 `current` 只在解码时变），并且**必须把 StartFrame
    喂给引擎**（回放泵与实机两条路径都要），引擎按"frame id 变化才清列表"的规则处理。
  - 漏掉的后果：一条消息少写一批像素，差值在后续帧累计（表现为"整帧大部分正确、某些 tile 局部不同"）。
- **重复合成用的 clip 必须取自 REGION 头，且必须是 FreeRDP 的那一份**：FreeRDP 用
  `region16_union_rect()` 把 region rects 合成 `clippingRects`，而它是**带合并**的（同一带内与
  unionRect 相交的项会被并成一个**跨越间隙的 bbox 矩形**）⇒ 要**直接调用 FreeRDP 的 region16 API**
  构造同一份集合，不要自己写"原始 rects 的并集"。
- **一条 Progressive 消息可以"有 REGION、0 个 tile"**（纯重复合成 pass，捕获里确实存在）：
  此时仍要用该 region 的 clip 重复合成整帧列表 ⇒ **clip 不能从 tile 推**（解析器要把 REGION 头的 rects
  单独回调出来）。
- **不要把多条消息的 decode 合并进一次 dispatch**（"帧内跨消息批量"已实测否决）：restamp（type 3）用 `cur`
  重建"本帧更早解码过的 tile"，其 `cur` **必须是该消息那一刻的值**；合并后无论排在同批之前还是之后都会与
  gdi 不一致。**并行度只能从"一条 stream 内部"找**。细节见 [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md) §2。
- **tile 网格公式照抄 FreeRDP**：`gridW = (w + (64 - w % 64)) / 64`，**不是** `(w + 63) / 64`。
  FreeRDP 在 16 对齐宽度是 **64 整数倍**时会**多算一格**（例如 3136 → 50 而非 49）。多出来的那圈 tile
  整块落在表面之外、不可能写出可见像素（与 region rects 的交集为空），但**"两个实现接受的 tile 集合
  不同"本身就是隐患**，照抄才能保证一致。
- `CreateSurface`：宽/高/scanline 按 **16 字节对齐**、**0xFF 初始化**、wire `0x20 → BGRX32` /
  `0x21 → BGRA32`。
- `SolidFill` 的 alpha 固定 `0xFF`；`SurfaceToSurface` 的 `destPts` 语义按 FreeRDP 实现照搬；
  `SurfaceToCache` 内部会嵌套调用 `EvictCacheEntry`，引擎侧要**抑制这次嵌套**；
  `MapSurfaceToScaledOutput` 本工程不支持（unmap、不合成，与 gdi 现状一致）。
- **RDPGFX 表面是持久的**：少实现一条命令，引擎与 gdi 就**永久分叉**。
- **不要给协议校验加上参考实现没有的上限**（"更严格"在这里等于"丢弃 gdi 会应用的消息"）：
  引擎曾自造 `numRects > 1024` / `numProgQuant > 16` 两条 region 头上限，于是把 gdi 会正常
  解码并合成的整条消息丢掉（表面从那一刻起永不自愈）。校验要么逐条照抄 FreeRDP（含它"只用
  字节预算兜底"的写法），要么把上限设成"这条消息的长度能容纳的量"；解码数据一律按声明数量
  定长。另一面：**头校验失败**两边都丢整条，而**读完部分 tile 之后才失败**时 FreeRDP 已经把
  读到的 tile 登记进本帧列表（`RfxParseStats::errorStage` 记的就是失败的阶段），两者不能混为一谈。

### 2.3 帧呈现（gdi 回退路径）

- 按**脏区**部分上传/呈现，**一律由 GPU 出屏**，CPU 只把脏区交给 GPU 一次：
  - 默认后端是 **Vulkan 呈现器**（`VkRenderer`）：CPU 帧把脏矩形写进 host-visible staging 缓冲
    （`vkCmdCopyBufferToImage`/`bufferRowLength` 只传该矩形），引擎帧直接给屏幕镜像 image；
    两者都由 GPU 做 letterbox blit → swapchain。**CPU 与引擎两条帧来源复用同一个类**。
  - 兜底后端是 **GLES 呈现器**（`GlesPresenter`，设备 Vulkan 不能上屏时用，实际主要是模拟器）：
    脏矩形用 ES3 + `GL_UNPACK_ROW_LENGTH` 传进桌面尺寸纹理，letterbox 由 shader 里的
    BGRA→RGBA swizzle + letterbox viewport 完成。
  - **不要**让 CPU 直接写窗口缓冲（曾经的"原生窗口缓冲呈现器"已删除）：窗口缓冲默认是 CPU 访问
    路径，文档明确写它"兼容性好但能与性能开销大"（见 [`native-libraries.md`](native-libraries.md) §6），
    而且 CPU 既要做 letterbox 又要写显示内存，比"把脏区交给 GPU"慢一个量级。
  - 后端选择用**独立的呈现能力判定**（`VulkanCapabilities::presenterSupported`），**比硬件解码的引擎判定宽松**
    （只要 device + `VK_OHOS_surface` + `VK_KHR_swapchain` + host-visible 内存，**不需要 compute**）。
- **swapchain 的尺寸与重建**（Vulkan 呈现器）：尺寸以 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 的
  `currentExtent` 为准，只有在它是 `UINT32_MAX` 时才用窗口尺寸 clamp；present 返回 `VK_ERROR_OUT_OF_DATE_KHR`
  就重建后跳过该帧、`VK_SUBOPTIMAL_KHR` 则先呈现再重建；**重建只重建"依赖变了的资源"**——
  swapchain/图像/视图/framebuffer/每图像信号量，而 render pass 只依赖**格式**、command pool/buffer 与
  每帧 fence/信号量都与尺寸无关，都要保留；旧 swapchain 通过 `oldSwapchain` 交回驱动复用。
  窗口尺寸变化**不是**故障，也不要为"等尺寸稳定"去延迟创建或主动重建。
- 两条后端的**共同约束**（与设备/分辨率无关）：只上传脏矩形；源行距可能被填充，所以要用调用方给的
  stride（GLES 侧配 `GL_UNPACK_ROW_LENGTH`）；通道顺序按目标面读出来的格式决定（屏幕面通常是 RGBA 序，
  FreeRDP 给的是 BGRA）；纹理/镜像重建后**首帧强制整幅**，否则其余部分会留空。
- **上屏只传逐条脏矩形，包围盒只作上限兜底（CPU 路线已启用）**：gdi 同时维护 `hwnd->invalid`（合并 box）和
  `hwnd->cinvalid[ninvalid]`（逐条矩形，见 `libfreerdp/gdi/region.c` 的 `gdi_InvalidateRegion`）；
  `PresentGdiFrame` 逐条上传，仅在**矩形数超过上限（256）**时退回包围盒（保证命令列表与拷贝区域数组有界）。
  一条矩形的帧就是 box，无需特判。
  - **为什么不留"只用包围盒"这条路（实测，同机同一批录像，CPU 路线 + `mode=fast`）**：
    滑动/碎片场景（657 帧）box 传 5591.6MB、单次上屏 4.32ms；逐条矩形传 1534.3MB（**−72.6%**）、
    上屏 2.89ms（**−33%**）、整帧计算 −6.7%，`fps` 无回退；看视频场景（195 帧）两者字节只差 1.8%、
    上屏 2.06 vs 1.99ms（噪声内）。即 **box 在任何场景都不更快**，而"阈值式自动选择"（矩形比 box 小 ≥25%
    才用矩形）在两个场景给出的结果与之一致，属于多余旋钮，已删除。
  - 客观量在回放 stats 的 `upload` 行长期保留：`uploaded=` 真正交给呈现器的字节，`(box=…)` 同一轮按盒
    上传的字节，`rectlist=used/total presents` 与 `truncated=`（超上限退回盒子的帧数）。
  - 两条后端都按"一次上传 + 多个拷贝区域 + 仍然只画一次 letterbox quad"实现（Vulkan 用紧凑拼接的 staging
    + 每个矩形一个 `VkBufferImageCopy`，GLES 每个矩形一次 `glTexSubImage2D`），**不要**为每个矩形各走一次
    present（那会把整屏 letterbox 画 N 遍）。像素一致性已核对：同一段录像两种上传方式跑完的最终画面
    逐像素无差异。
  - **GPU 路线：逐条合成已实现，但默认关闭（`Impl::kComposeRects = false`，仍合成 bbox）**。引擎同样逐条
    收集脏矩形（`MarkSurfaceDirty`：Progressive 每个解码 tile 一条，ClearCodec/缓存/填充/未压缩各按自己的
    rect），`Compose()` 用 `CoalesceRects` 合并成**精确矩形**（同行同跨度先并、再同列并，并集不变），
    上限 256、超限退回 bbox；`Stats()` 的 `compose copies/rects/maxRects/overflow` 是这条列表的账
    （实测：滑动 `rects=10587 maxRects=113`、视频 `rects=1422 maxRects=106`，都 `overflow=0`，
    即逐条合成在这两份录像上都不需要退回 box）。
    **关闭的原因**：真正逐条合成会稳定暴露一处像素分叉——滑动样本 **156 px @ (992,1728)-(1007,1791)**
    （`bad=1`，复跑同一 bbox；box 模式 `bad=0`）。已定性到**传输可见性**，不是解码/合成语义错：

    - **引擎表面是对的**：把差异像素从**三处**取回——屏幕镜像（`ReadScreen`）、表面的 CPU 映射
      （invalidate 前/后）、以及**设备侧**对同一地址的读（`CmdCopyBuffer` 拷回）——CPU 与设备读到的字节
      都等于 gdi 主缓冲，即"表面内容正确、只是没被拷上屏"。比较侧探针（`GfxReplay::CompareFrames` 的
      `ProbeMismatchPixels`，只在 `bad` 帧读映射、不做设备读回）长期保留：一次跑完就能把
      "合成漏拷" 与 "表面内容就不同" 分开。
    - **区域几何是对的**：覆盖这些像素的那条 region 的 `imageOffset`/`imageExtent` 与
      `bufferOffset = top*stride + left*4` 都等于期望值（逐像素 1:1），没有错位/裁剪问题。
    - **失败粒度是缓存行**：差异恰是 **一个 64 字节（= `nonCoherentAtomSize` = 16 px）宽的列 × 该帧被写的行**，
      不是"某个矩形整块"——即失败粒度属于宿主缓存行，不属于 rect。
    - **box 为什么掩盖它**：box 的 hull 会在**后续帧**再次覆盖同样的像素，于是"某帧读到陈旧表面"会被
      后一帧补上；rect 列表一个像素只在被标记时合成一次，陈旧一次就**永久**留下。所以 box/rect 的差别
      不是"box 拷得对"，而是 **rect 列表把这类一次性的陈旧合成放大成永久分叉**（§6 的"分叉放大器"）。
    - **已实测排除**：多 region 合批（改成每条 region 一次 `vkCmdCopyBufferToImage` 结果不变）；
      宿主写"更早 flush"（每次 CPU 写后立刻 `FlushMappedMemoryRanges`，即 §1 的急切做法）结果不变；
      标记漏覆盖（差异像素在被写那一帧确实在其 rect 列表里）；区域几何/源偏移；解码/内容差异。
    - **只减轻未根治**：把复合前的 barrier 换成**无条件** `ALL_COMMANDS → TRANSFER`（不依赖标志位与传递性）
      后 156 px → 60 px（残下一条 1 px 宽列），说明这是**排序/可见性缺口**且该平台并未完全遵守；
      **不要**据此认为"加个 barrier 就好了"。
    - **量测纪律**：引擎内的一次性探针（探针自身会 flush/读回，即额外提交）会**改变失败形状**
      （156 px → 60 px）。定位这类问题不能在被测路径里加读回；用比较侧探针（只读映射）或
      独立的设备读回探针。
    - **已实测：换 coherent host-visible 类型不是答案**（`hostMemType=(0,DL|HV|CO) coherent=1`）：同样的
      156 px 仍在，另外多出一处 192 px 的块。即"cached 类型的 flush/invalidate"不是根因。
    - **"换一条读回通道对照"在这台设备上不可用**：把屏幕镜像 blit 进 host-visible **linear** 镜像再读回
      （图形路径 = presenter 的读法），同一区域的 diff 不是变小而是变大（156 → 1281），说明这条通道自身
      就被污染（与该平台 `vkCmdBlitImage`/`vkCmdClearColorImage` 已知不可靠一致，见 §5）。
      因此 **"结果错（上屏看到的就不同）" 与 "只是读回陈旧（只有影子对比被骗）" 目前无法用设备内读回区分**。
    - 要判定"上屏结果是否真的不同"，只剩：① 用 **compute（采样）**读回屏幕镜像；② 外部对窗口截图
      （需要能冻结在出差的帧）。两者都还没做。

  - **上屏成本账（实测，滑动样本 657 帧，`mode=fast` + `Vulkan对比`；`presentSplit` / `composeArea`
    两行是长期保留的账）**：每帧 `compose=320µs / flush=11393µs / blit=1760µs`，present 合计 ≈13.5ms
    （整帧 ≈62ms 的 22%）。⇒ **上屏策略能动的只有约 1ms/帧**：
    - 脏区拷贝的**带宽**：`composeArea rects/box=0.295`，即 box 每帧多拷 ≈9.1MB，按测得 ≈22GB/s 折
      ≈0.56ms/帧（这一项只由"拷多少字节"决定，与是否逐条无关）；
    - **录制开销**：逐条比 box 多 +199µs/帧（519 vs 320µs）；
    - 剩下 **11.4ms/帧（85%）是 flush = CPU 在等 Progressive 解码 kernel**
      （`gpuMs rlgr+idwt ≈ 11.3s / 513 chunk ≈ 22ms/帧`），上屏侧怎么改都动不了它。
    - 同数据 A/B：present 13.47ms（box） vs 13.09ms（逐条），即逐条净赚 ≈0.4ms/帧（0.6%）；端到端
      `feed` 41.6s vs 42.3s（落在复跑噪声内）。
    - **口径结论**：**GPU 侧不存在"逐条几乎总是更快"**——CPU 侧那套成立是因为逐条砍掉的是
      **host→device** 字节（实测 −72.6% 字节 / −33% 上屏）；GPU 侧这些字节本来就在设备内，同样的
      −70% 字节只换来 ≈0.5ms/帧，还要付出录制开销与**分散小矩形的目的端写**。所以上屏侧 box 与逐条
      的差别 ≤1ms/帧，**逐条不是为性能留的**，当前默认 box 即可（正确、少 region、录制更省）。
    - **唯一的结构性上屏收益是全幅 blit**：`blit=1760µs/帧`（≈2.8%）无条件发生（屏幕镜像 → swapchain
      的 letterbox blit）。要动它只能"只把脏区提交给 swapchain"（incremental present，或按 swapchain
      图像各维护一份"已上传"状态），优先级排在解码优化之后。
    CPU 侧不受影响：`hwnd->invalid` 本来就是同一批 `gdi_InvalidateRegion` 调用的 bbox，其矩形列表与 box
    覆盖范围等价。
- **两条后端的分工必须完全一致**（否则就是一条快一条慢）：**CPU 只把脏区交出去一次，不做任何像素变换**
  ——通道交换、缩放、letterbox 一律在 GPU 侧；`glTexSubImage2D`/staging 上传只做行拷贝。Vulkan 侧因此用
  **一个小 quad（采样 + fragment shader 换通道序 + dynamic viewport 做 letterbox）**而不是
  `vkCmdBlitImage`：blit 不能换通道序，在 CPU 上逐字节换序会把上屏变成瓶颈（这是实测过的反面做法）。
- **host-visible staging 的 flush 只覆盖本帧写入的字节**（`size` = 实际写入量，不是 `VK_WHOLE_SIZE`）：
  该缓冲会按见过的最大脏区增长，整块 flush 会把本帧没碰过的内存一起提交。
- **不要再叠加 present-on-change**：静止态已由 FreeRDP 的失效区门控保证
  （`HmrdpBeginPaint` 把 `hwnd->invalid->null` 置 TRUE，只有真正执行绘制原语时 `gdi_InvalidateRegion`
  才置 FALSE，`HandleEndPaint` 对 `null` 直接返回）——额外 `memcmp` 只增加内存/带宽开销。

## 3. 性能规则（真机口径）

- **先量内存类型，再谈算法**：CPU 侧像素命令的成本由所选 host-visible 类型决定（见 §1），
  差一个类型就是 20~180 倍。真机上有内存探针（`ProbeHostMemory`：写/连续拷贝/跨行拷贝三种形状）。
- **分离"同步点固定开销"与"GPU 真的在跑"**：先测空提交（`ProbeSubmitCost`，实测 ~0.6ms/次），
  否则会把 GPU 执行时间误判成同步开销，做出完全相反的设计。
- **不要每命令排空流水线 / 等待设备**（旧实现在每条命令末尾 `glFinish` 是反面教材）：GPU 侧用 barrier，
  只在真正需要 CPU 回读处同步；**不要立即销毁在飞资源**（fence 延迟回收）。
- **compute 派发注意并行度与访存形态**，不只是每轴工作组上限（`maxComputeWorkGroupCount`，本机 65535）：
  一个 workgroup 只覆盖 512~1536 个 invocation、且每个 invocation 串行处理 4096 个元素
  （RLGR 位流逐字节 refill、tile 逐像素循环）时，GPU 利用率极低——此时"引擎比 FreeRDP 的 CPU 软解还慢"
  是必然结果。整屏矩形需要 10 万+ 工作组，必须用 2D/3D 网格而不是线性下标。
  **这条不是"性能建议"而是正确性要求**：`rfx_compose`（一个 invocation 一个像素，线性派发
  `tileCount*64` 个工作组）在整屏 Progressive 消息上需要 65535 以上（实测最大 91008），超限后平台
  **静默只执行一部分** —— 消息尾部的 tile 不再被合成、表面/画面停在上一次内容，表现为"下半屏整块
  stale"（全屏比对里是 `bbox` 覆盖整屏 + 下半屏 100% 像素错误）。现在主机按 65535 拆 2-D
  网格、着色器用 `gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x` 还原线性 index；
  `Stats()` 的 `composeGridSplits`/`composeGroupsMax` 若不为 0 就说明这条路径正在生效。
- **任何带 `barrier()` 的 kernel，早退必须由整个 workgroup 一致决定**：`if (gid >= uNumStreams) return;`
  这类按 lane 早退会让最后一个 workgroup 的 `barrier()` 只被执行一部分 → **未定义行为**
  （实测表现为花屏 + present 失败）。要么在早退前先 `barrier()` 收敛，要么把参数补齐到整组。
- **`rfx_decode.comp` 是寄存器极度敏感的 kernel**：实测**只加两个计数器 + 一个 binding**就慢 2x。
  因此**不要在一个 kernel 里堆路径**（type 0/2/3 三套逻辑同文件），优先**按类型拆成多条 kernel**，
  每条只保留一条代码路径；改它的门槛指标是**每 stream 的 GPU 时间**（`gpuMs rlgr` / stream 数）。
- **不要用"跳过某条 dispatch + 差值反推"做归因**：跳过会改变后续数据相关的负载与依赖，实测偏差可达
  数倍，而且会渲染出花屏、容易被误判为回归。用 timestamp query 直接量（§6）。
- **不要用一小段"看起来固定"的耗时推断瓶颈**：先看它**是否随输入量变化**（本工程的 decode 每 chunk 耗时
  确实随码流字节量浮动），再下结论。
- **不要在非目标设备上标定性能**：真机口径要压的是**同步点数 / 驱动调用数 / CPU 介入次数**。
- **只做标准能力探测，不做标准 API 的行为自检**：自检只针对我们自己的语义与算法
  （`hmrdp_vk_context.*` 只探测能力）。
- 已实测**不成立**的两个 RLGR 优化假设：① payload 读的"次数/合并"（加 32bit 字缓存：无变化）；
  ② payload 读的**延迟**（整段搬进 shared：kernel 只快 17%，而 16KB shared 把常驻 workgroup 压到
  每 SM 2 个、整系统反而慢 3 倍）。⇒ 现在的瓶颈是**分歧型串行位解码在 SIMT 上的低效率**，
  不是访存，也不是靠微调着色器能追回来的。下一步的设计见
  [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。

## 4. 关键实现要点（与引擎配套的会话侧约束）

- FreeRDP 的 RDPGFX 回调必须接上 `freerdp_client_OnChannelConnectedEventHandler`（绑到
  `ChannelConnected`/`ChannelDisconnected`），否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
- live 的 GPU 接管由会话侧驱动；保留**双渲染影子对照**（引擎 vs gdi）作为运行时体检。
- 引擎屏幕经 `GfxVkDesktop::Compose()` + `VkRenderer` 上屏（目前只用于回放/对比路线）；GPU 接管时
  "本机解码耗时"计 0（解码已在 GPU），含义见 [`session-and-input.md`](session-and-input.md) 的遥测。

## 5. 历史包袱（**不要在新代码里依赖**）

- **只有一套引擎：Vulkan**（不要再引入 GLES/EGL；`hmrdp` 原生库不链接 `EGL`/`GLESv3`）。设备能否跑引擎由
  Vulkan 侧回答（`GetVulkanCapabilities()` / `vulkanInfo`，见 §6）；采集/回放的 GFX 解析与命令映射
  （`hmrdp_gfx_driver.*`）、容器解析器 `ParseRfxProgressive` 与 ClearCodec hook 保留。
- 已移除的 H.264/AVC 支持：它在真机上只有客户端广告 AVC444 时才被服务端启用（属微软非核心可选项），
  且命中硬解也无明显收益（瓶颈在解码后的 CPU 环节）⇒ 整体砍掉，服务端改用其他码流
  （Progressive / ClearCodec 等）。**不要再引入媒体库依赖**。

## 6. 验证回路（跨实现保留）

```
设置页「抓取 RFX 码流（测试）」  →  hmrdp_gfx.bin（原始 ZGX 字节，u32 长度 + payload）
dev 页「回放测试」三条路线：CPU / Vulkan / Vulkan对比
```

- **验收口径**：`Vulkan对比` 路线（引擎 vs 离线 gdi 桌面逐像素比对）的
  `compare(GPU vs gdi): checks=… bad=0 rgbPx=0 maxDelta=0`。
  每 30 帧采一次（`kCompareEvery`），`bad` = 采样的帧里有多少帧与 gdi 不一致；目标 **`bad=0`**。
  同一行还带 `alphaPx`（只差未使用的 alpha 字节，不算视觉差异）、`bbox`（差异包围盒）与
  `smallDeltaPx`（≤2 的"舍入级"像素数）——`rgbPx` 从 0 变成几百万而 `bbox` 覆盖整屏，说明是**没画**，
  而不是"画得略有不同"。
- **改完引擎 / 着色器 / FreeRDP 补丁，两份录像都要跑**（都必须是 `bad=0 rgbPx=0`）：
  `.cache/hmrdp_gfx.bin`（浏览/滚动：消息稀疏、脏区小）是**回归门**；
  `.cache/hmrdp_gfx_video.bin`（看视频：整帧大块变化、每帧 4~7 条 Progressive 消息）是另一个场景。
  当前两份都过：浏览 `checks=21`（`frames=659`）、视频 `checks=6`（`frames=198`）。
- **上屏成本账长期保留**（判断"该优化哪一段"用）：引擎 `Stats()` 的 `composeArea`（每帧逐条并集面积 vs
  box 面积，比值决定"少拷多少字节"）与 `presentSplit`（present 拆成 `compose` 录制 / `flush` 提交+等待 /
  `blit` 全幅上屏三段的每帧平均值），回放页 stats 里是 `present=` 一行；CPU 路线对应的账是 `upload=` 行。
- **先看计数，再加日志**：`Stats()` 里的 `rfxParse`（`errors` 必须为 0）、`rejectedTiles`/`restamped`/
  `skippedRegions`/`batchOverflow`/`composeGridSplits` 就是"协议级行为有没有按预期发生"的账本，
  绝大多数分叉靠它们就能定位到"哪条消息/哪条命令没做"。**不要为了查一次分叉就新加一次性日志探针**：
  周期性统计行会很快冲掉缓冲区，而且没有 `%{public}` 标记的参数会被打成 `<private>`
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）。
- **三条路线的呈现方式**：`Vulkan`/`Vulkan对比` 由引擎 `Compose()` 出屏幕镜像后 `VkRenderer` 上屏；
  `CPU` 路线的 gdi 帧走同一个呈现器接口（`CreateFramePresenter()`：Vulkan 优先，GLES 兜底）——与 live
  会话是同一条路径，所以两者不会各自分叉。
- **引擎性能归因三件套（dev，只在真机跑）**：
  1. `ProbeHostMemory`：逐个 host-visible 内存类型的写/连续拷贝/跨行拷贝带宽 → 决定 CPU 侧像素命令的成本（§1）；
  2. `ProbeSubmitCost`：空 command buffer 的 submit+fence 往返（实测 ~0.5ms）→ 用来区分"同步点固定开销"与
     "GPU 真的在跑"；没有它就会把 GPU 执行时间误判成同步开销；
3. **每条 dispatch 的 GPU 时间靠 timestamp query 直接量**（`rfx_decode`/`rfx_idwt`/`rfx_compose` 各一对
   `vkCmdWriteTimestamp`，fence 等待后 `vkGetQueryPoolResults` 读回，统计在引擎 `Stats()` 的 `gpuMs …` 行）。
   两端都取 `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`；**不要用"跳过某条 dispatch + 差值反推"**（见 §3）。
- **分叉怎么定位**：先看整屏比对（`Vulkan对比` 的 `bad/rgbPx/bbox/maxDelta`）。**判定分叉只以 gdi 自己的
  表面/主缓冲为准**（历史上用手写镜像做过对比，它会误报）。需要收敛到"哪条命令"时，按
  **"引擎跳过/多做了一条命令"→"两边对同一条命令算得不一样"**两个方向分开查：前者看引擎自己的
  `Stats()` 计数（`rfxParse errors`、`skippedRegions`、`rejectedTiles`…）与"引擎这条命令到底做没做"，
  后者才是解码/合成语义。**不要用"跳过某条 dispatch + 差值反推"做归因**（见 §3）。
- **不要用"脏区/覆盖集合"比对来判定或定位分叉**（实测教训）：参考实现（gdi）的脏矩形是
  **region16 合并出来的粗超集**——同一帧可能只有一条跨越整个屏幕的 rect——所以"参考覆盖了、引擎没覆盖"
  在**像素完全一致**的帧上也会大量出现（实测：box 合成 `bad=0`，同一套对照却报出 138 帧不一致；rect
  合成 `bad=1` 只报 1 帧）。覆盖比较**既不能作为"漏标记"的证据，也不能用来定位**，只会把人带偏。
  判定/定位只能靠**像素值**：把差异矩形的像素从三处同时打出来——**引擎屏幕（`ReadScreen`）/ 参考的
  主缓冲（`primary_buffer`）/ 参考的表面（`GetSurfaceData()->data`）**，一次就能区分
  "引擎合成漏了这块"（表面 == 主缓冲 ≠ 引擎屏幕）、"参考主缓冲自己陈旧"（表面 ≠ 主缓冲）与
  "解码内容不同"（引擎表面 ≠ 参考表面）。**推论（更一般）**：用一个"更细粒度"的语义（如逐条矩形）替换
  "更粗"的语义（如 bbox）时，先把它当**分叉放大器**跑一遍门禁——粗语义会把差异静默盖住，换细语义时才
  暴露出来；这比事后排查便宜，也是这类改动必须过 `bad=0` 的原因。
- **量测纪律（否则数字不可比）**：
  - **两种回放节拍**（`mode=` 字段，dev 页「节拍」按钮切换）：
    - **`mode=fast`（默认）**：每个呈现帧给一个 `kFrameMs` 周期，**预算覆盖整帧**（解码 + 命令应用 +
      上屏），不是只包住 present；**跨帧不做任何补偿或追赶**——超时的帧保留它更长的周期，提前完成的帧
      补睡余量。这样汇总数字才是回放的**如实**描述（"由后续帧还清"会让平均值命中目标，而个别帧被测时的
      节拍并不一样，参考性就没了，实际流畅度也不会变好）。实际周期 = 预算 + 平台唤醒误差，所以 `fps`
      会略低于名义值。**`fps` 因此是吞吐量口径**（"这些字节喂进来，客户端吃不吃得下"），不是 live 的
      出帧率。
    - **`mode=realtime`（新）**：按**录制时记录的到达时刻**喂数据（`GfxReplayPump` 的每 record 节拍），
      即复现 live 当时的**帧间隔**。帧间隔本身就是负载的一部分（决定 CPU 频率/大小核落点、几十 MB 表面 +
      bitmap cache 的缓存局部性、以及 WinPR 线程池 worker 的唤醒代价），所以"客户端能不能扛住真实负载"
      必须看这个模式。此时 `fps` ≈ 当时的出帧率，可直接与 live 工具栏读数对照；**`lag=`** = 落后录制
      日程的最大值（= 扛不住的量），**不做追赶**（追赶会把后续间隔压缩，报出一个实际没发生过的节拍）。
    - **realtime 仍不是闭环**：live 的节奏由服务端与**每帧 `RDPGFX_FRAME_ACKNOWLEDGE`** 共同决定（ack 在
      `EndFrame` 返回后才发，而我们的上屏就在 `EndFrame` 里），回放既不发 ack 也没有服务端调度。因此
      realtime 复现的是"服务器已经产出的那份流的到达节奏"，不是"服务器面对一个更快的客户端会怎么发"。
      版本 0（无时间戳）的旧录像没有到达时刻，`realtime` 会自动回落 `mode=fast` 并在日志里说明。
  - **比较上屏成本用 `present=`**，比较"解码 + 上屏"合计用 `feed=`（同样已剔除启动；两种模式都剔除节拍
    睡眠）。
  - 回放页的「路线 / 重新回放」按钮内部都是 `stopReplayTest()` + 重新 `start`，**在一轮还没跑完时点击
    等于把那一轮掐断**；性能数字只取 `(running=0)` 的**整轮**，且要记下 `frames=` 以确认是整份跑完
    （不同录制的帧数不同，不能假定某个固定值）。
  - 判定"跑完"要**轮询** `replayTestStats` 文本里的 `(running=0)`，不要用固定 sleep：早取会拿到中途值，
    晚取白等。
  - `uitest dumpLayout` 的输出**不是合法 JSON**（部分字符串编码后 `ConvertFrom-Json` 会报错），
    用正则抓 `route=…(running=N)` 这类文本即可。
  - 性能探针（`ProbeHostMemory` / `ProbeSubmitCost`）是**进程内一次**（`RunDeviceProbes` 的 `call_once`）：
    每次切路线都会重建引擎，逐次重探只会拖慢启动并给数字加噪声。
- **采集内容与格式**：单文件 `hmrdp_gfx.bin`，存的是**服务端在 GFX 通道上、ZGFX 之前**的原始字节；
  采集点在 `rdpgfx_on_data_received` 的 `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册
  （见 [`native-libraries.md`](native-libraries.md) §3.7）。两种布局：
  - **v1（当前）**：文件头 = 8 字节 magic `HMRDPGX1`，之后每条 = `u32 长度` + `u64 到达时刻(µs)` +
    原始字节。到达时刻由 app 侧在回调里取（宿主单调时钟），**不需要改 FreeRDP 补丁**。
  - **v0（旧录像）**：无 magic，每条 = `u32 长度` + 原始字节（无时间信息）⇒ 只能 `mode=fast`。
  `GfxCaptureHasTimestamps()` 只读文件头判断布局，所以 dev 页可以在打开整份文件前决定 realtime 是否可用。
  录制文件**不入库**：设备端在应用沙箱，本地副本放 gitignore 目录；dev 的抓取开关与「硬件解码」有联动
  （录制期间走软解），见设置页实现。
  **已知扰动**：抓取钩子在 RDP 线程上每条 chunk 取一次锁 + 两次 `fwrite`，而 live 的码流形态是闭环的
  （客户端快慢会影响服务端怎么发），所以录制本身会把被测对象拖慢一点；要求更高的保真度时要把落盘挪出
  RDP 线程。
- **把某份捕获喂给回放**：`dev 页「回放测试」`读的是**应用 filesDir 里的 `hmrdp_gfx.bin`**
  （设备侧固定路径 `/data/app/el2/100/base/com.lixa.hmrdp/haps/entry/files/`，用
  `hdc file send` **覆盖已存在的那个文件**——`hdc` 不能在该目录里新建文件）。
  所以换样本 = 覆盖同一个文件再点「重新回放」（该按钮会重新读文件）；
  **同名文件的不同录制不是同一份流**，不能互相背书，换之前把旧的 recv 回 `.cache/` 留档。
- **样本集（本地 `.cache/`，不入库）**：性能结论**必须分场景给**，且**每份捕获都要先自己过一遍
  `Vulkan对比 bad=0` 才能当基线**（同名文件的不同录制之间不能互相背书）：
  - **浏览/滚动**类：消息稀疏、脏区小，瓶颈在 GPU kernel；
  - **看视频**类：整帧大块变化（`diff`/restamp 占比高）、每帧整屏脏，瓶颈先被**上屏/提交结构**盖住。
  - **判据要看"每 stream 的 GPU 时间"与当时在飞的 lane 数**，而不是 dispatch 的总耗时——后者随输入
    字节量变化，"两个场景的平均值接近"并不说明大头是固定等待。
  - 单条 Progressive 消息可带**数千条 stream**，而能同时在飞的 workgroup 数量有限，所以**平均值会低估**
    "当前结构下"的并行度上限。
- **`Vulkan对比` 的现状**：两份录像都是 `bad=0 rgbPx=0 maxDelta=0`
  （浏览/滚动 `checks=21`、看视频 `checks=6`）。**任何一轮性能结论的前提是那一轮 `bad=0`。**
- **对比结果与呈现路径解耦**：对比读的是引擎屏幕镜像（`ReadScreen()`）与离线 gdi 主缓冲，呈现器只碰
  swapchain/present，所以**换呈现后端、改重建策略都不会影响 `bad` 的判定**；反过来说，`bad` 变化只能来自
  解码/合成。

- **差分测试（补齐捕获里没有的码流）**：统一方法 = **同一份载荷**分别喂 FreeRDP 解码器与我们的实现，
  逐像素比对。载荷优先用 FreeRDP 自带编码器生成；边界要覆盖**尺寸非 64 倍数**、纯色/渐变/UI 文本/alpha。
  ClearCodec 只有解码（运行时必须支持）没有编码器 ⇒ 只能用真实捕获；`CAPROGRESSIVE_V2` 双方都未实现 ⇒
  对齐"检测到即不支持"。
- 采集与回放需要**打过补丁并重编的 FreeRDP**（见 [`native-libraries.md`](native-libraries.md) §3.7）。

## 7. 待办

- **GPU 侧逐条合成：修掉传输可见性缺口，然后打开 `Impl::kComposeRects`**（§2.3）。已知：
  - 复现：同一份滑动样本 `bad=1`、**156 px @ (992,1728)-(1007,1791)**（复跑同一 bbox）；box 模式 `bad=0`；
    只有 `kComposeRects=true` 才出现（"分叉放大器"效应，见 §6）。
  - **已定性**：不是解码/合成语义错——差异像素在**引擎屏幕 / 表面的 CPU 映射 / 表面的设备侧读**
    三处里，后两者都等于 gdi 主缓冲；覆盖它们的 region 几何（`imageOffset`/`imageExtent`、
    `bufferOffset = top*stride + left*4`）与期望值完全一致；失败粒度是**一个 64 字节
    （`nonCoherentAtomSize` = 16 px）宽的列 × 该帧被写的行**，即"传输拷贝读到宿主刚写的缓存行时是陈旧的"。
    box 之所以干净，是因为它会**在后续帧再合成同一批像素**把陈旧值补掉；rect 列表一像素只合成一次，
    陈旧即永久（详见 §2.3）。
  - 已排除（都实测过）：多 region 合批（每条 region 单独一次 `vkCmdCopyBufferToImage` 结果不变）；
    宿主写急切 flush（每次 CPU 写后立刻 `FlushMappedMemoryRanges` 结果不变）；标记被
    `ResetGraphics`/`MapSurfaceToOutput`/`MapSurfaceToScaledOutput` 丢弃（曾逐个打点验证，全 0）；
    `ReadScreen()` 未 flush（它先 `Flush()`）；`CoalesceRects` 丢矩形（并集精确）；引擎那几条直写路径
    （`ClearCodecDecode`/`UploadBgra`/`SolidFill`/`SurfaceToSurface`/`CacheToSurface`）漏标记
    （都是"写入 rect == 标记 rect"）；Progressive 漏标记（按**整 64×64 tile** 标记，是 tile 内裁剪
    子写入的超集）；覆盖比较法（§6 已证否，不要再走）。
  - 只减轻未根治：复合前换成**无条件** `ALL_COMMANDS → TRANSFER` barrier，156 px → 60 px（残下 1 px 宽列）。
    即"该平台不保证宿主写对 transfer 读的可见性"，而 barrier 不能被假定为已经修好。
  - **已实测排除（第二轮）**：把表面换成 **coherent** host-visible 类型（`coherent=1`，引擎不再做任何
    flush/invalidate）——同样的 156 px 仍在，另多一处 192 px 块；把屏幕镜像 blit 进 host-visible
    **linear** 镜像再读回做对照——同一区域的 diff 反而更大（156 → 1281），该对照通道自身被污染，
    在这台设备上**不能**用来判"结果错 vs 只是读回陈旧"。
  - **量测纪律**：引擎内的一次性读回探针会改变失败形状（156 px → 60 px），禁用；定位只用比较侧探针
    （`GfxReplay::CompareFrames` 的 `ProbeMismatchPixels`：`bad` 帧里从引擎表面取回像素，
    只读映射、不做设备读回，输出 `surface==primary / surface==screen / noSurface` 计数）。
  - **上屏策略已定量（见 §2.3 的成本账）**：上屏侧 box 与逐条 rect 的差别 ≤1ms/帧（逐条净赚
    ≈0.4ms/帧，端到端在噪声内），而每帧 13.5ms 的 present 里 11.4ms 是在等 Progressive 解码 kernel。
    ⇒ **上屏侧就用 box（`kComposeRects=false`，当前默认）**：它正确、region 少、录制更省，本身就是
    只拷标记的 hull（不是整屏）。**不要**为了对齐 CPU 侧的逐条上传去打开 `kComposeRects`。
  - **上屏侧真正的收益在全幅 blit**（`blit=1760µs/帧`≈2.8%，无条件发生）：要做就做"只把脏区交给
    swapchain"（incremental present / 按 swapchain 图像维护已上传状态），优先级在**解码 kernel 并行化**
    （[`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)，22ms/帧 GPU）之后。
  - 逐条 rect 只有在**换成非 transfer 的合成路径**（compute 写屏幕镜像，见 §1）后才有意义，且只值得
    在"拷贝带宽成为主成本"的场景（高分辨率 + 稀疏更新 + 解码不再是大头）；顺手用它做"上屏结果是否真的
    不同"的判定（compute 采样读回，或冻结帧 + 外部窗口截图）。改完两份录像都要 `bad=0` 再打开。
- **RLGR 解码 kernel 的并行化重设计**（producer/consumer，含已修/未解问题与实现要点）：
  单独成文 → [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。
- **换样本复验**：不同分辨率（特别是宽/高为 **64 整数倍**的）、含**多条 REGION**消息的捕获。
  每份新捕获都要自己过一遍 `bad=0` 才能当基线（同名文件的不同录制之间不能互相背书）。
- **UI 收尾**：GPU 回放入口的置灰（`DeviceCapabilities` 的 Capability 模式，给出原因）——
  「硬件解码」已完成（`DeviceCapabilities.hardwareDecode()`，见
  [`native-libraries.md`](native-libraries.md) §6）。
- **把 Vulkan 引擎接进 live 会话**（当前只有回放/对比跑引擎；live 一律走 gdi + 呈现器）。届时
  「硬件解码（RFX）」设置项才真正生效（是否可用的判据取 `vulkanInfo` / `GetVulkanCapabilities()`）；
  在此之前它只是被保留、不参与决策。
- **呈现能力判定在模拟器上的口径**：现在模拟器一律回落 GLES 呈现器（与"GPU 只在真机"一致）；若以后要让
  模拟器用 Vulkan 上屏，只需改 `FillVerdicts()` 里那一处 emulator 分支。
