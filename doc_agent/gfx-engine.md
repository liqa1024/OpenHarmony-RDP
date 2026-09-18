# GFX 码流 / GPU 桌面引擎

本文件是**改 GFX 相关代码前必读**的口径文档。目标：引擎与 FreeRDP 自己的 gdi 软解**逐像素一致**
（验收见 §6）。CPU（gdi）链路的成本与优化见本文件 §8，
上屏管线见 [`present-pipeline.md`](present-pipeline.md)，解码 kernel 的后续工作见
[`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md)。

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
- 像素操作只有四类：**解码写入 / 纯色填充 / 表面拷贝 / 缓存存取**；最终都是"把某矩形内的像素写成
  确定值"，写进**同一张表面图**，多张表面再合成到**可见桌面图**。

### 0.3 各码流（本工程范围）

| codecId | 名称 | 解码 | 写入方式 | 读旧像素 | 跨命令状态 |
|---|---|---|---|---|---|
| 0x0000 | Uncompressed | 无（直接拷贝） | 目标矩形整块覆写 | 否 | 无 |
| 0x0008 | **ClearCodec** | 有（`clear_decompress`） | 目标矩形，**可能只覆盖部分像素** | **是** | **会话级**（序列号 + 字形/竖条缓存） |
| 0x0009 | CAPROGRESSIVE | 有 | 64×64 分块 + 有效区域、多次细化 | 区域外保留 | **每块持久状态**（本工程主用） |
| 0x000A | Planar | 未实现 | — | — | 检测到即回退 |
| 0x000D | CAPROGRESSIVE_V2 | **双方都未实现** | — | — | 检测到即不支持 |

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

## 1. 码流分工

- **Progressive / 未压缩位图 / 表面绘制 → GPU**（SPIR-V compute + transfer），引擎直写表面缓冲；
  Progressive 的解码拆成**两个 kernel**（`rfx_decode.comp` + `rfx_idwt.comp`）：
  1. `rfx_decode`：一条 lane 一条 (tile,component) stream，跑 RLGR/去量化/差分与持久状态同步；
  2. `rfx_idwt`：**一个 workgroup 一条 stream**，把该 stream 的 4096 个系数搬进 **shared memory**
     再跑三级逆 DWT，全局只读一次写一次。
     **为什么必须拆**：这个变换原本在全局 SSBO 上做多趟 16bit 读-改-写，是当时 decode 的最大头；
     搬进 shared 后 GPU 时间显著下降。语义（子带偏移/长度、每步 INT16 截断、差分顺序、位状态）与 §2.1
     逐条一致，参考画面的 `bad=0` 仍是门禁（§6）。
- **`rfx_compose` 必须按像素并行**（一个 lane 一个像素）：按 tile 并行时 32 个 lane 会写 32 个不同 tile
  （每 4 个有效字节占一条 cache line），且每像素还要遍历整条裁剪 rect 列表；改成按像素 + 由 host 把
  裁剪 rect 预先算成**tile 内局部坐标**存进 tileMeta 后，该 dispatch 快了约两个数量级。
- **ClearCodec 留在 CPU**：它不是自包含的（未覆盖像素保留原值），复用 FreeRDP 的 `clear_decompress`
  对**持久映射的表面缓冲**做读改写（共享内存，**不搬 GPU、不做逐区域跨侧往返**）。
- **表面/缓存存储**是**持久映射的 host-visible 线性缓冲**（屏幕仍是 image），于是 CPU 访问零成本，
  不需要 staging / 回读 / 布局状态机。
- **host-visible 类型必须选 `HOST_CACHED`**（若设备提供）：`DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`
  是 uncached 的，CPU 读只有百 MB/s 量级，而引擎所有 CPU 像素命令（ClearCodec、cache 存取、
  `SurfaceToSurface`、填充、未压缩上传）都是"读+写映射"⇒ 会直接变成瓶颈；同一设备上有 `HOST_CACHED`
  类型时可快一到两个数量级。
  **约束**：cached 类型不是 `HOST_COHERENT`，所以 CPU/GPU 交接必须显式做**范围级缓存维护**——
  CPU 写过的映射在 submit 前 `vkFlushMappedMemoryRanges`，fence 等待后、CPU 读之前
  `vkInvalidateMappedMemoryRanges`（按行切 range、对齐 `nonCoherentAtomSize`）；
  CPU 触碰一片 rect 之前也要先 invalidate 该 rect（部分行写入不能把陈旧邻居写回）。
  CPU-only 的缓冲（bitmap cache 项）只被 CPU 访问，不需要任何维护。
- **CPU 写过的表面被 GPU 读取前必须补 `HOST → TRANSFER` barrier**（UMA 不等于免费）。

开关：全局「硬件加速」管的是**上屏后端**（开 = Vulkan 呈现器，关 = GLES 且不碰 Vulkan），
**不是**这里说的引擎；解码在 live 一律走 gdi。**当前没有引擎接进 live 会话**（引擎只跑回放/对比），
将来接管时，口径是 引擎关 / 无 Vulkan / 引擎初始化失败 ⇒ 回退 **gdi**（见 §7）。

> **「真机专属功能」**：GPU 引擎与 GPU 回放只在真机上验证与使用；**模拟器不参与**（其 Vulkan 实现会按
> 标准接口谎报能力）。模拟器上不要开硬件加速、不要跑 GPU 回放，也不要拿模拟器结论约束真机行为。

## 2. 必须保留的协议/算法语义（**与后端无关**，改引擎/着色器前逐条对照）

这些不是"实现细节"，而是与 FreeRDP 对齐的**行为契约**。少一条，引擎与 gdi 就会分叉。

### 2.1 解码层

- **RLGR 必须 64 位读位**；零游程用 `1<<k` 累加，`kp` 上限 80、`>>3` 得 `k`。
- 去量化 `shift = quant + progQuant − 1`；`shift == 0` 或 `>= 16` **不改动**该子带
  （FreeRDP 的移位函数在这两种情况下直接返回）。
- `RFX_TILE_DIFFERENCE`：与持久 `current` 做**饱和加法**，并把结果**同时写回** `current`。
- UPGRADE 的 SRL 与 raw **两个位流同时活跃**：非 LL 子带走 raw（按 `sign` 决定符号），LL3 走 SRL。
- 每 `(tile, 分量)` 的 `current` / `sign` / `bitPos` **跨消息常驻**；`bitPos` 每次解码都写为新的
  `quant + progQuant`。位状态缓冲**每流整字节对齐**（不要紧排：相邻流会共享 32 位字、丢 RMW 更新）；
  引擎的条目里每个分量 3 个 `bitPos`，故 stride 是 12 字节而非 10。
- **WBT 块状态机是"每条消息"的**：`progressive_decompress` 开头把状态清零，所以"REGION 在 FRAME_BEGIN
  之前 / FRAME_END 之后"这条忽略规则**只看本条消息**里的块（把 `frameBegin/frameEnd` 跨消息持久化会
  接受 gdi 跳过的 region）。**被忽略的 region 连同它的 tile 状态更新一起不生效**。
- **畸形消息的拒收粒度照抄 FreeRDP**：region 头/分量量化表校验失败 ⇒ 整个 region 什么都不做（两边
  一致）；但**"读了这么多 tile"之后才失败**（tile 头长度、块字节数、`numTiles` 不一致）时，FreeRDP
  已经把读到的 tile **登记进本帧的 tile 列表**（并更新其元数据），只是不进入解码/合成。UPGRADE 的
  声明长度与实际不符时，FreeRDP 在**已把 refinement 累加进 `current`/`sign` 之后**返回失败
  ⇒ 该 tile 的像素保持旧值但系数状态已变。
  - 引擎实现：UPGRADE 的这条拒绝判定由解码着色器逐流算出，写在**该流位状态条目的闲置字节**里，
    合成时读它 ⇒ 任一分量被拒就**整块不合成**；判定在"kFirst 解码成功"时清除（type 3 重合成不清），
    于是旧像素一直保持到该 tile 重新解码成功。**改位状态缓冲布局时必须同步这两处**。
  - 判定的记账口径是**"请求过的位数"**（FreeRDP 的 `BitStream::position`：越界也照加，
    对 ≥32 的位移不计数），不是"流里实际有多少位"。
- **UPGRADE 的 `numBits` 会 BYTE 回绕**：`numBits = tile->yBitPos − yBitPos` 在"本次比该 tile 上次更粗"
  （`ob < nb`）时回绕成接近 256 的大值；此时 FreeRDP 对 ≥32 的位移**什么都不做**，而读出的值仍按硬件
  5 位掩码取向 ⇒ **读到垃圾且不消耗位**，长度校验照样通过。把它钳成 0（=跳过该带）或真去消耗 n 位
  **都会分叉**。
- 逆 DWT 的抽取/尾块与带偏移/长度必须照抄（抽取路径的起点序列是固定表；非抽取路径不同，不要混用）。
  ⚠ **参考侧（gdi）在 `-DWITH_SIMD=ON` 下走 FreeRDP 自己的 NEON 抽取支**，与上面这套标量算术差
  ±1 量级（它的半加在 16 位车道上做）；引擎要与它比，就得一并接受这个量级，或与**当前构建配置**对齐
  ——口径见本文件 §8.4。
- 颜色转换：`yCbCrToRGB` 的 `(y+4096)<<16` + 乘系数后 `>>21`，系数是 **float 截断**得到的整数，
  不要用浮点现算。

### 2.2 合成层（**最容易出错、也最"看不见"的部分**）

- **compose 必须按桌面尺寸/region clip 裁剪**：桌面宽高非 64 倍数，边缘 tile 若不丢像素会按 stride
  折回下一行、污染邻接 tile。
- **`update_tiles` 的「同帧图块重复合成」必须实现**：FreeRDP 每收到一条 Progressive 消息，都会用
  **本消息的 clippingRects** 合成 **"本帧至今解码过的全部 tile"**（该列表只在**帧 id 变化**时清零，
  即 RDPGFX StartFrame），而不只是本消息的 tile。
  - 引擎做法：每条消息为"本帧更早的 tile"补一遍合成（从持久 `current` 重跑逆 DWT——**不需要缓存 tile
    像素**：逆 DWT 是确定性的，而 `current` 只在解码时变），并且**必须把 StartFrame 喂给引擎**
    （回放泵与实机两条路径都要），引擎按"帧 id 变化才清列表"的规则处理。
  - 漏掉的后果：一条消息少写一批像素，差值在后续帧累计（表现为"整帧大部分正确、某些 tile 局部不同"）。
- **重复合成用的 clip 必须取自 REGION 头，且必须是 FreeRDP 的那一份**：FreeRDP 用 region16 的合并
  接口把 region rects 合成 `clippingRects`，而它是**带合并**的（同一带内相交的项会被并成一个**跨越
  间隙的 bbox 矩形**）⇒ 要**直接调用 FreeRDP 的 region16 API** 构造同一份集合，不要自己写"原始 rects
  的并集"。
- **一条 Progressive 消息可以"有 REGION、0 个 tile"**（纯重复合成 pass，捕获里确实存在）：
  此时仍要用该 region 的 clip 重复合成整帧列表 ⇒ **clip 不能从 tile 推**（解析器要把 REGION 头的 rects
  单独回调出来）。
- **不要把多条消息的 decode 合并进一次 dispatch**：restamp 用 `cur` 重建"本帧更早解码过的 tile"，
  其 `cur` **必须是该消息那一刻的值**；合并后无论排在同批之前还是之后都会与 gdi 不一致。
  **并行度只能从"一条 stream 内部"找**，细节见 [`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md) §2。
- **tile 网格公式照抄 FreeRDP**：`gridW = (w + (64 − w % 64)) / 64`，**不是** `(w + 63) / 64`。
  宽度是 64 的整数倍时上游会**多算一格**。多出来的那圈 tile 整块落在表面之外、不可能写出可见像素，
  但**"两个实现接受的 tile 集合不同"本身就是隐患**，照抄才能保证一致。
- `CreateSurface`：宽/高/scanline 按 **16 字节对齐**、**0xFF 初始化**、wire `0x20 → BGRX32` /
  `0x21 → BGRA32`。
- `SolidFill` 的 alpha 固定 `0xFF`；`SurfaceToSurface` 的 `destPts` 语义按 FreeRDP 实现照搬；
  `SurfaceToCache` 内部会嵌套调用 `EvictCacheEntry`，引擎侧要**抑制这次嵌套**；
  `MapSurfaceToScaledOutput` 本工程不支持（unmap、不合成，与 gdi 现状一致）。
- **RDPGFX 表面是持久的**：少实现一条命令，引擎与 gdi 就**永久分叉**。
- **不要给协议校验加上参考实现没有的上限**（"更严格"在这里等于"丢弃 gdi 会应用的消息"）：
  自造的 `numRects` / `numProgQuant` 上限会把 gdi 会正常解码并合成的整条消息丢掉（表面从那一刻起
  永不自愈）。校验要么逐条照抄 FreeRDP（含它"只用字节预算兜底"的写法），要么把上限设成"这条消息的
  长度能容纳的量"；解码数据一律按声明数量定长。注意"头校验失败"两边都丢整条，而"读完部分 tile 之后
  才失败"时 FreeRDP 已登记读到的 tile，两者不能混为一谈（解析统计里记的就是失败的阶段）。

### 2.3 帧呈现（gdi 回退路径）

- 按**脏区**部分上传/呈现，**一律由 GPU 出屏**，CPU 只把脏区交给 GPU 一次：
  - 默认后端是 **Vulkan 呈现器**（`VkRenderer`）：CPU 帧把脏矩形写进 host-visible staging 缓冲
    （`vkCmdCopyBufferToImage` + `bufferRowLength` 只传该矩形），引擎帧直接给屏幕镜像 image；
    两者都由 GPU 做 letterbox blit → swapchain。**CPU 与引擎两条帧来源复用同一个类**。
  - 兜底后端是 **GLES 呈现器**（设备 Vulkan 不能上屏时用，实际主要是模拟器）：脏矩形用 ES3 +
    `GL_UNPACK_ROW_LENGTH` 传进桌面尺寸纹理，letterbox 由 shader 里的 swizzle + letterbox viewport 完成。
  - **不要**让 CPU 直接写窗口缓冲：窗口缓冲默认是 CPU 访问路径，开销大，而且 CPU 既要做 letterbox 又要
    写显示内存，比"把脏区交给 GPU"慢一个量级。
  - 后端选择用**独立的呈现能力判定**，**比引擎判定宽松**（不需要 compute，
    见 [`native-libraries.md`](native-libraries.md) §6）。
- **swapchain 的尺寸与重建**（Vulkan 呈现器）：尺寸以 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 的
  `currentExtent` 为准，只有在它是 `UINT32_MAX` 时才用窗口尺寸 clamp；present 返回
  `VK_ERROR_OUT_OF_DATE_KHR` 就重建后跳过该帧、`VK_SUBOPTIMAL_KHR` 则先呈现再重建；
  **重建只重建"依赖变了的资源"**——swapchain/图像/视图/framebuffer/每图像信号量，而 render pass 只依赖
  **格式**，command pool/buffer 与每帧 fence/信号量都与尺寸无关，都要保留；旧 swapchain 通过
  `oldSwapchain` 交回驱动复用。窗口尺寸变化**不是**故障，也不要为"等尺寸稳定"去延迟创建或主动重建。
- 两条后端的**共同约束**（与设备/分辨率无关）：只上传脏矩形；源行距可能被填充，所以要用调用方给的
  stride（GLES 侧配 `GL_UNPACK_ROW_LENGTH`）；**通道顺序由目标面的图像格式承担**，
  **CPU 不做任何像素变换**——通道交换、缩放、letterbox 一律在 GPU 侧，上传只做行拷贝。
  Vulkan 侧因此用**一个小 quad（采样 + fragment shader 换通道序 + dynamic viewport 做 letterbox）**
  而不是 `vkCmdBlitImage`（blit 不能换通道序，在 CPU 上逐字节换序会把上屏变成瓶颈）。
  纹理/镜像重建后**首帧强制整幅**，否则其余部分会留空。
- **上屏传逐条脏矩形还是包围盒**：
  - **CPU 路线**：gdi 同时维护合并 box 与逐条矩形列表，`PresentGdiFrame` 逐条上传，仅在**矩形数超过
    上限**时退回包围盒（保证命令列表与拷贝区域数组有界）。这里**逐条明显更快**——它砍掉的是
    **host→device** 字节（实测字节数降七成以上、上屏耗时降三成），且 box 在任何场景都不更快，
    所以"阈值式自动选择"属多余旋钮，已删除。客观量在回放 stats 的 `upload` 行长期保留
    （`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
  - **GPU 路线**：逐条已实现但**默认关闭**（合成 bbox）。原因：设备内的字节本来就在 GPU 上，
    同样的降幅只换来 <1ms/帧，还要付出录制开销与分散小矩形的目的端写。**逐条不是为性能留的**，
    默认 box（正确、少 region、录制更省）即可。
    另有一条平台侧证据：在本平台开逐条会在**转移路径**上暴露可见性缺陷——**逐条只合成每个像素一次，
    一次陈旧读就永久保留**，而 box 会在后续帧把同一批像素重合成一遍、把陈旧读盖住。
    所以这条路径在把合成移出 transfer 路径之前不要开启。
  - 两条后端都按"一次上传 + 多个拷贝区域 + 仍然只画一次 letterbox quad"实现，**不要**为每个矩形各走
    一次 present（那会把整屏 letterbox 画 N 遍）。
- **host-visible staging 的 flush 只覆盖本帧写入的字节**（`size` = 实际写入量，不是 `VK_WHOLE_SIZE`）：
  该缓冲会按见过的最大脏区增长，整块 flush 会把本帧没碰过的内存一起提交。
- **不要再叠加 present-on-change**：静止态已由 FreeRDP 的失效区门控保证（未执行绘制原语时失效区为
  空，直接返回）——额外 `memcmp` 只增加内存/带宽开销。
- **present 那一段的实现、帧槽与设备侧握手、picture ping-pong、CPU vs GPU 耗时对比与后续工作清单**：
  单独成文 → [`present-pipeline.md`](present-pipeline.md)。

## 3. 性能规则

**判据与账目**：这条线只按"每帧计算成本 = 能耗"取舍（fps 只要够用）；成本结构、账目口径与量测纪律见
本文件 §8。以下是与引擎/GPU 强相关的规则。

- **先量内存类型，再谈算法**：CPU 侧像素命令的成本由所选 host-visible 类型决定（见 §1），
  差一个类型就是一到两个数量级。真机上有内存探针（`ProbeHostMemory`：写/连续拷贝/跨行拷贝三种形状）。
- **分离"同步点固定开销"与"GPU 真的在跑"**：先测空提交（`ProbeSubmitCost`），否则会把 GPU 执行时间
  误判成同步开销，做出完全相反的设计。
- **不要每命令排空流水线 / 等待设备**（每条命令末尾 `glFinish` 是反面教材）：GPU 侧用 barrier，
  只在真正需要 CPU 回读处同步；**不要立即销毁在飞资源**（fence 延迟回收）。
  但"少等"不等于"不等"：**主机写进 mapped 表面之前，必须等"上一帧那次提交"跑完**——它的合成
  （`vkCmdCopyBufferToImage`）正在**读**这些表面缓冲；主机写落在拷贝中途，那一帧的 picture 与屏幕
  就是两帧的混合。合成是 transfer 不是 dispatch，所以"compute 在飞"盖不住它，而"整帧只有 CPU 侧命令"
  （ClearCodec / 缓存 / SolidFill / SurfaceToSurface / 未压缩上传）的帧连一个 dispatch 都没有。
  这笔账按**提交**记、不是按命令记（`SyncForCpuAccess` 的 `submissionSinceHostDrain`：每个提交窗口
  只等一次，纯 Progressive 帧不付），顺序约束见 [`present-pipeline.md`](present-pipeline.md) §1。
- **compute 派发注意并行度与访存形态**，不只是每轴工作组上限（`maxComputeWorkGroupCount`）：
  一个 workgroup 只覆盖少量 invocation、且每个 invocation 串行处理数千个元素（RLGR 位流逐字节
  refill、tile 逐像素循环）时，GPU 利用率极低——此时"引擎比 CPU 软解还慢"是必然结果。
  整屏矩形需要十万级工作组，必须用 2D/3D 网格而不是线性下标。
  **这条不是"性能建议"而是正确性要求**：线性派发的 per-pixel kernel 在整屏 Progressive 消息上会超过
  每轴上限（实测最大数万个），超限后平台**静默只执行一部分** —— 消息尾部的 tile 不再被合成、表面/画面
  停在上一次内容，表现为"下半屏整块 stale"。现在主机按上限拆 2-D 网格、着色器用
  `gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x` 还原线性 index；
  `Stats()` 的 `composeGridSplits`/`composeGroupsMax` 不为 0 就说明这条路径正在生效。
- **任何带 `barrier()` 的 kernel，早退必须由整个 workgroup 一致决定**：按 lane 早退会让最后一个
  workgroup 的 `barrier()` 只被执行一部分 → **未定义行为**（实测表现为花屏 + present 失败）。
  要么在早退前先 `barrier()` 收敛，要么把参数补齐到整组。
- **`rfx_decode.comp` 是寄存器极度敏感的 kernel**：实测只加少量计数器/binding 就慢一倍。因此
  **不要在一个 kernel 里堆路径**（type 0/2/3 三套逻辑同文件），优先**按类型拆成多条 kernel**，
  每条只保留一条代码路径；改它的门槛指标是**每 stream 的 GPU 时间**。
- **不要用"跳过某条 dispatch + 差值反推"做归因**：跳过会改变后续数据相关的负载与依赖，实测偏差可达
  数倍，而且会渲染出花屏、容易被误判为回归。用 timestamp query 直接量（§6）。
- **不要用一小段"看起来固定"的耗时推断瓶颈**：先看它**是否随输入量变化**，再下结论。
- **不要在非目标设备上标定性能**：真机口径要压的是**同步点数 / 驱动调用数 / CPU 介入次数**。
- **只做标准能力探测，不做标准 API 的行为自检**：自检只针对我们自己的语义与算法
  （`hmrdp_vk_context.*` 只探测能力）。
- 已实测**不成立**的两个 RLGR 优化假设：① payload 读的"次数/合并"（加 32bit 字缓存：无变化）；
  ② payload 读的**延迟**（整段搬进 shared：只快十几个百分点，而大块 shared 把常驻 workgroup 数压下来、
  整系统反而慢数倍）。⇒ 现在的瓶颈是**分歧型串行位解码在 SIMT 上的低效率**，
  不是访存，也不是靠微调着色器能追回来的。下一步设计见
  [`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md)。

## 4. 关键实现要点（与引擎配套的会话侧约束）

- FreeRDP 的 RDPGFX 回调必须接上 `freerdp_client_OnChannelConnectedEventHandler`（绑到
  `ChannelConnected`/`ChannelDisconnected`），否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
- live 的 GPU 接管由会话侧驱动；运行时体检走**参考画面对比**（不再跑引擎 vs gdi 的双渲染影子，那套已删）。
- 引擎屏幕经 `Compose()` + `VkRenderer` 上屏（目前只用于回放/对比路线）；GPU 接管时"本机"里的
  **decode 段计 0**（解码已在 GPU），`zgx+parse`/`compose`/`present` 三段照旧，
  含义见 [`session-and-input.md`](session-and-input.md) §3。

## 5. 已移除的方向（**不要在新代码里重新引入**）

- **只有一套引擎：Vulkan**（不要再引入 GLES/EGL 引擎后端）；设备能否跑引擎由 Vulkan 侧回答
  （`GetVulkanCapabilities()` / `vulkanInfo`，见 §6）。采集/回放的 GFX 解析与命令映射
  （`hmrdp_gfx_driver.*`）、容器解析器与 ClearCodec hook 保留。
- 已移除的 H.264/AVC 支持：它在真机上只有客户端广告 AVC444 时才被服务端启用（属微软非核心可选项），
  且命中硬解也无明显收益（瓶颈在解码后的 CPU 环节）⇒ 整体砍掉，服务端改用其他码流。
  **不要再引入媒体库依赖**。

## 6. 验证回路（跨实现保留）

```
设置页「抓取 RFX 码流（测试）」  →  原始码流文件（u32 长度 + payload，见下）
dev 页「回放测试」：路线:CPU / 硬件加速   参考:关 / 导出 / 对比
```

- **路线只有两条，且是同一条解码路径的两种上屏后端**：`CPU` = gdi 解码 + GLES 上屏（完全不碰 Vulkan）、
  `硬件加速` = gdi 解码 + Vulkan 上屏（就是设置里的「硬件加速」，页面上的按钮与它是同一个值）。
  **GPU 引擎那两条路线（引擎单跑 / 引擎 vs gdi 影子对比）已从 UI 撤掉**（引擎在做重做，
  [`gpu-accel-plan.md`](gpu-accel-plan.md)）：实现仍在 `GfxReplayRoute::kVulkan*` 里、只有从代码调用才跑得到。
- **验收口径 = 参考画面（golden reference）**：同一条路线先 `参考:导出` 记一遍
  （`hmrdp_ref_<captureTag>.hash` + `.bmp`，`captureTag` 是录制内容的指纹，**换一份录像就换一套参考**），
  之后每轮 `参考:对比` 报
  `ref compare: refFrames=… checks=… bad=0 firstBad=-1 imageFrame=… rgbPx=0 maxDelta=0`：
  `bad` = 逐帧哈希与参考不同的帧数（目标 **0**），`rgbPx/maxDelta/bbox` 是参考**图像那一帧**的逐像素差
  （用来判断"哪一帧、差多少"，不是全集）。失败时会把那一帧 dump 成 `…_fail.bmp`。
  - **它取代了"引擎 vs gdi"的影子对比**：影子要求引擎与 gdi 两条实现始终一致，实际上做不到长期维护；
    参考画面只要求"输出不变"，与是 CPU 还是 GPU 出的无关——所以**引擎重做后也用同一套参考**。
  - 前提是**解码逐位等价**（本文件 §8.4）⇒ 参考长期有效。
   - ⚠ **参考模式不是性能模式**：它每帧对整个桌面做一次哈希（`ref export/compare` 行报 `hashMs=`），
     这段时间落在 gdi 的 `EndFrame` 里、会算进 `compose` ⇒ 读性能数字时用 `参考:关` 那一轮。
   - ⚠ **参考的口径跟着构建走**：解码侧允许舍入级改动（`WITH_SIMD` 开关、逐位等价之外的 SIMD 改写）
     ⇒ 改完必须**重录**参考，此后 `bad=0` 只表示"自那次重录起没有再变"（回归门）；改动的量级由
     `dwt check: … maxDelta=` 与 `rgbPx/maxDelta` 给（本文件 §8.4）。
   - **每轮的 `state=` / `run=`**：stats 文本第一行是 `state=running|finished|aborted  run=<n>`，
     多轮测量按它等待与归属，别按内容猜；驱动脚本见 `native/scripts/replay-rounds.ps1`。

- **两份基线录像，改完解码/引擎/着色器/补丁都要跑**：一份**浏览/滚动**（消息稀疏、脏区小）作回归门，
  一份**看视频**（整帧大块变化、每帧多条 Progressive 消息）覆盖另一场景。样本与参考文件都在本地
  gitignore 目录，设备侧用同一个固定文件名喂给回放（`hdc` 只能**覆盖**已存在的文件，不能在该目录新建）。
  **每份新捕获先自己 `参考:导出` + `参考:对比` 过一遍才能当基线**（同名文件的不同录制不能互相背书——
  参考文件名里的指纹就是为此）。
- **先看计数，再加日志**：`Stats()` 里的解析错误计数、`rejectedTiles`/`restamped`/`skippedRegions`/
  `batchOverflow`/`composeGridSplits` 就是"协议级行为有没有按预期发生"的账本，绝大多数分叉靠它们
  就能定位到"哪条消息/哪条命令没做"。**不要为了查一次分叉就新加一次性日志探针**：
  周期性统计行会很快冲掉缓冲区（见 [`build-and-verify.md`](build-and-verify.md) §5.1）。
- **呈现方式**：CPU 路线的 gdi 帧走呈现器接口（`硬件加速` 开 = Vulkan、关 = GLES）——与 live 会话是
  同一条路径，所以两者不会各自分叉；引擎路线（代码里仍在）由引擎 `Compose()` 出屏幕镜像后走同一个
  呈现器。**参考判定与呈现路径解耦**：参考哈希读的是 gdi 主缓冲（引擎路线是屏幕镜像，两者内容相同），
  `present` 只碰 swapchain，所以换呈现后端、改重建策略都不影响 `bad`；反过来说 `bad` 变化只能来自
  解码/合成。**任何一轮性能结论的前提是那一轮 `bad=0`。**
- **影子对比（引擎 vs gdi 同跑逐帧比像素）已经**从代码里**删掉**（连同驱动里的 per-PDU 交织喂送）：
  它要求两条实现长期一致，维护成本大于收益。定位"差在哪一帧、差多少"现在靠参考对比的
  `rgbPx/alphaPx/maxDelta/bbox`（只覆盖参考图像那一帧）；要逐帧定位就重新 `参考:导出` 再比。
  引擎侧的参考核对需要读回引擎画面（`GfxVkDesktop::ReadScreen` 保留着，见
  [`gpu-accel-plan.md`](gpu-accel-plan.md) §5 M1）。
- **引擎性能归因三件套（dev，只在真机跑）**：
  1. `ProbeHostMemory`：逐个 host-visible 内存类型的写/连续拷贝/跨行拷贝带宽 → 决定 CPU 侧像素命令的成本；
  2. `ProbeSubmitCost`：空 command buffer 的 submit+fence 往返 → 区分"同步点固定开销"与"GPU 真的在跑"；
  3. **每条 dispatch 的 GPU 时间靠 timestamp query 直接量**（每条 kernel 一对 `vkCmdWriteTimestamp`，
     fence 等待后读回，统计在 `Stats()` 的 `gpuMs …` 行）。两端都取 `COMPUTE_SHADER` 阶段；
     **不要用"跳过某条 dispatch + 差值反推"**（见 §3）。
- **分叉怎么定位**：先看整屏比对（`bad/rgbPx/bbox/maxDelta`）。**判定分叉只以 gdi 自己的表面/主缓冲
  为准**（用手写镜像代替它会误报）。需要收敛到"哪条命令"时，按**"引擎跳过/多做了一条命令"→
  "两边对同一条命令算得不一样"**两个方向分开查：前者看引擎自己的计数与"这条命令到底做没做"，
  后者才是解码/合成语义。
- **不要用"脏区/覆盖集合"比对来判定或定位分叉**：参考实现（gdi）的脏矩形是 region16 合并出来的
  **粗超集**——同一帧可能只有一条跨越整个屏幕的 rect——所以"参考覆盖了、引擎没覆盖"在**像素完全一致**
  的帧上也会大量出现。覆盖比较**既不能作为"漏标记"的证据，也不能用来定位**。
  判定/定位只能靠**像素值**：把差异矩形的像素从三处同时取回——**引擎屏幕 / 参考的主缓冲 /
  参考的表面**，一次就能区分"引擎合成漏了这块"、"参考主缓冲自己陈旧"与"解码内容不同"。
  **推论（更一般）**：用一个"更细粒度"的语义替换"更粗"的语义（如逐条矩形替换 bbox）时，
  先把它当**分叉放大器**跑一遍门禁——粗语义会把差异静默盖住，换细语义时才暴露出来。
  这比事后排查便宜，也是这类改动必须过 `bad=0` 的原因。
- **两种回放节拍**（dev 页切换，`mode=` 字段）。跨轮次比较的前提（频率必须一致、纯 CPU 侧的 A/B 要用
  CPU 受限的样本）见本文件 §8.3：
  - **`mode=fast`（默认「跑满」）**：完全不打节拍——上一帧做完就喂下一条，**`fps` 就是吞吐上限**，
    不是 live 的出帧率。
  - **`mode=realtime`**：按**录制时记录的到达时刻**喂数据，复现 live 的**帧间隔**（帧间隔本身是负载的
    一部分：CPU 频率/大小核落点、缓存局部性、线程池唤醒代价）。此时 `fps` ≈ 当时的出帧率，可直接与
    live 工具栏读数对照；`lag=` = 落后录制日程的最大值（= 扛不住的量），**不做追赶**（追赶会报出实际
    没发生过的节拍）。无时间戳的旧录像自动回落 `fast` 并在日志里说明。
  - **realtime 仍不是闭环**：live 的节奏由服务端与**每帧帧回执**共同决定（回执在我们的上屏之后才发出），
    回放既不发回执也没有服务端调度。它复现的是"服务器已经产出的那份流的到达节奏"，不是"服务器面对一个
    更快的客户端会怎么发"。**它的 `fps` 是录制节拍本身，不能当"客户端能力"读**——判定客户端占了多少
    只看三个数：`feed=`（同一轮的纯计算，节拍睡眠已剔除）、`lag=`、以及 `fast` 的 `fps`（吞吐上限）。
  - **时间戳取在"整条 DVC chunk 重组完成之后、ZGX 解压之前"**，所以相邻两条的 `gap` =
    **客户端处理前一条的时间 + 等服务端送下一条的时间**；当相邻两条"背靠背"到达（客户端手里有货）时，
    `gap` 退化成前一条的**服务时间上界**。典型观测形状：到达侧被塑形成**持续带宽 + 突发额度的令牌桶**
    （逐条速率与载荷大小基本无关），于是 `fps ≈ 带宽预算 / 每帧字节数`——
    **"低 fps"是到达侧的字节预算问题，不是解码/上屏算力问题**。
  - **判据**：live 每秒的 `perf:` 行与工具栏一起看即可当场判定——**本机 ≈ 1000/fps** 就是客户端触顶；
    本机 ≪ 帧周期、duty 只有个位数百分比 ⇒ 客户端在等数据，闸门在到达侧。
- **live 与回放的逐相对照**（"本机为什么 live 比回放慢"唯一可靠的判定方式）：两者的 `perFrame`/`本机`
  **同源同义**（同一个计量器、同一批钩子、同样的分母），可以并排比。
  **判读顺序**：① 先比 `kB/frame`、`cmds/frame`——低 fps 的 live 每帧扛的是累积变化，内容不同就没有
  可比性；② 内容对得上再比相位：`decode` 差得多 ⇒ 频率/缓存/被别的活抢 CPU；`zgx+parse` 差得多
  ⇒ 解压/解析；`compose`/`present` 差得多 ⇒ 合成/上屏。**同一份录像用 `realtime` 回放**是把两者对齐的
  标准做法；`fast` 只能给吞吐上限。
- **回放页的「路线 / 重新回放」内部都是 `stop + start`，在一轮没跑完时点击等于把那一轮掐断**；
  性能数字只取 `(running=0)` 的**整轮**，并记下 `frames=` 确认整份跑完（不要假定固定帧数）。
  判定"跑完"要**轮询** stats 文本里的 `(running=0)`，不要固定 sleep。
  `dumpLayout` 的输出**不是合法 JSON**，用正则抓文本即可。
- **性能探针（内存/提交成本）是进程内一次**：每次切路线都会重建引擎，逐次重探只会拖慢启动并给数字加噪声。
- **采集内容与格式**：单文件存的是**服务端在 GFX 通道上、ZGFX 之前**的原始字节，采集点在
  `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册（见 [`native-libraries.md`](native-libraries.md) §3）。
  两种布局：
  - **v1（当前）**：文件头 = 8 字节 magic，之后每条 = `u32 长度` + `u64 到达时刻(µs)` + 原始字节。
    到达时刻由 app 侧在回调里取（宿主单调时钟），**不需要改 FreeRDP 补丁**。
  - **v0（旧录像）**：无 magic，每条 = `u32 长度` + 原始字节（无时间信息）⇒ 只能 `fast`。
  只读文件头即可判断布局，所以 dev 页能在打开整份文件前决定 realtime 是否可用。
  录制文件**不入库**（设备端在应用沙箱，本地副本放 gitignore 目录）。
  **已知扰动**：抓取钩子在 RDP 线程上每条 chunk 取锁 + 落盘，而 live 的码流形态是闭环的
  （客户端快慢会影响服务端怎么发），所以录制本身会把被测对象拖慢一点；要求更高的保真度时要把落盘
  挪出 RDP 线程。
- **差分测试（补齐捕获里没有的码流）**：统一方法 = **同一份载荷**分别喂 FreeRDP 解码器与我们的实现，
  逐像素比对。载荷优先用 FreeRDP 自带编码器生成；边界要覆盖**尺寸非 64 倍数**、纯色/渐变/UI 文本/alpha。
  ClearCodec 只有解码（运行时必须支持）没有编码器 ⇒ 只能用真实捕获；`CAPROGRESSIVE_V2` 双方都未实现
  ⇒ 对齐"检测到即不支持"。
- 采集与回放需要**打过补丁并重编的 FreeRDP**（见 [`native-libraries.md`](native-libraries.md) §2/§3）。

## 7. 待办

- **GPU 硬件加速的推进顺序与形状**（tile 解码 + 合成做成"载荷进、像素出"的**一个阶段**；
  相位适配性、交接成本、M0–M4 里程碑与出口）：单独成文 → [`gpu-accel-plan.md`](gpu-accel-plan.md)。
  RLGR producer/consumer 的实现约束仍在 [`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md)（old）。
- **CPU（gdi）链路的多核与平台适配**（换池/换任务队列、producer/consumer 流水线、worker 数按 duty
  自适应）：单独成文 → [`cpu-accel-plan.md`](cpu-accel-plan.md)（阶段二）。单核部分的知识见本文件 §8。
- **把 Vulkan 引擎接进 live 会话**（当前只有回放/对比跑引擎；live 一律走 gdi + 呈现器）。届时需要一个
  **引擎开关**与"引擎初始化失败 ⇒ 回退 gdi"的口径；它与现有的「硬件加速」（只管上屏后端）是两回事，
  不要复用同一个设置项。整体顺序与出口见 [`gpu-accel-plan.md`](gpu-accel-plan.md) §5/§7。
- **呈现能力判定在模拟器上的口径**：现在模拟器一律回落 GLES 呈现器（与"GPU 只在真机"一致）；
  若以后要让模拟器用 Vulkan 上屏，只需改 `FillVerdicts()` 里那一处 emulator 分支。

## 8. CPU（gdi）链路：成本结构、账目与量测纪律

**CPU 路线** = FreeRDP gdi 解码 + 我们自己的呈现器；它与 §1 的 GPU 引擎是**两条线**。这一节是这条线的
**通用知识**：成本怎么读、账怎么记、门禁是什么、哪些估算被否证过。下一步要做的事（多核与平台适配）
见 [`cpu-accel-plan.md`](cpu-accel-plan.md)；历史口径与旧数据见 [`cpu-path_old.md`](cpu-path_old.md)（old）。

### 8.1 判据与账目口径

- **判据**：**把每帧的计算成本降下来 = 能耗降下来**；fps 只要"够用"（跟得上服务端的到达节奏、不卡）。
  判收益看每轮 `cpu=`（进程 CPU 时间）、`本机` 拆相、GPU/DRAM 流量；**不看**单纯墙钟。
  - ⚠ **`cpu=` 只在同一频率档下可比**：动态功耗 ≈ f·V²，多核把 SoC 压到低频档时，同一秒 CPU 时间
    更便宜 ⇒ "并行换了多少 CPU 秒"**不是能量结论**；要么在同一 `cpuKHz=` 档下比，要么按
    "CPU 时间 × 档位"估，要么直接看 duty 下的功耗。
- **优先"少干活"**（去冗余拷贝、少唤醒、去掉重复搬运），但**不是因为"并行不好"**：多核低频在很多负载下
  比单核高频更省电，**并行本身是正当的能效手段**。这条线的并行形态（平台任务队列、宽度、相位归属、
  内存布局）见 [`cpu-accel-plan.md`](cpu-accel-plan.md)：**并行对能耗是赚的**（整屏样本的 `E2` 约为串行的
  一半）。
- 回放 stats 就是这条线的账：

  | 行 | 含义 |
  |---|---|
  | `perFrame 本机=… = zgx+parse + decode + compose + present  (+ sync … blocked)` | **本机 = 处理时间**；各拆相相加**等于**本机，`sync` **不计入**（见下） |
  | `prog ms/frame: read / dispatch / dec(blocked) / update  (calls= unions= tiles= tilesDec=)` | `decode` 的内部：`read` 读输入位流、`dispatch` 投递 tile（并行才有）、**`dec` = tile 解码段**、`update` = `update_tiles` 整段（其中像素拷贝默认已随解码段并行，见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §0/§2）；`calls` 消息数、`unions`/`tiles` = `update_tiles` 的并集次数与被访问 tile 数、`tilesDec` = 真正解码的 tile 数 |
  | `prog2 ms/frame (sampled 1/16, n=…): rlgr / dequant+diff / idwt / state / upgrade / color  sum=` | **`dec` 之内的拆相**（1/16 采样探针）；`state` = 系数状态拷贝（`sign`/`current`）、`color` = `yCbCrToRGB` + 写 tile |
  | `run … threads=… cpu=…s  cpuKHz=…` | 该轮的 worker 数与**整轮进程 CPU 时间**；`cpuKHz` 是本轮拿到的 SoC 频率档 |
  | `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp` | **非像素 GFX 命令**；它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 前先看这行** |
  | `gfx setup: <Name> took … us` | 单条结构命令（模式切换/整面清零这类卡顿） |
  | `uploaded/box/rectlist/truncated`、`present=` | 上屏侧：实际交给呈现器的字节、帧时间（细节见 [`present-pipeline.md`](present-pipeline.md)） |
  | `ref export: …` / `ref compare: refFrames=… checks=… bad=… firstBad=… rgbPx=… maxDelta=… hashMs=…` | **参考画面对比**（golden reference，见 §6 与 8.4）：`bad` = 与参考逐帧哈希不同的帧数；`rgbPx/maxDelta/bbox` 只是参考图像那一帧的逐像素差 |
  | `dwt check: tiles=… mismatch=… maxDelta=…` | **解码侧改动的对拍**（`参考:对比` 那一轮按 1/16 采样，见 8.4） |

- **`sync` = 等 GPU 放开主缓冲**（`BeginDesktopBufferWrite`；CPU 路线让解码器直接写呈现器缓冲，所以
  **本帧第一次写之前**要等上一帧的 GPU 拷贝读完，等待点见 [`present-pipeline.md`](present-pipeline.md) §1）。
  它是**阻塞**不是处理 ⇒ 单列；**帧的整段墙钟 = `本机` + `sync`**（回放再加节拍睡眠）。
  - ⚠ **这笔等待天然落在 `zgx+parse` 的窗口里**：帧首钩子（`StartFrame` / 帧内第一条表面命令）在
    `AccountChunkPrefix()` **之前**跑，而抓取的"一条记录"通常就是一整帧的 ZGX 段 ⇒ 该窗口覆盖了这次等待。
    所以测点必须把它交出来（`OnBlockedBeforeFrameWork()`）：不交，`本机` 就把它算两遍、在 GPU 成为慢的
    一侧时虚高到 `sync` 那么多。自检：**`本机 + sync ≈ 1/fps`**——右边明显小于左边就是被算重了。
- **`pace` 是唯一直接剔除的项**：回放的人为节流不是客户端工作。
- **判读顺序**：① `setup` ② `kB/frame`/`cmds/frame`（内容是否可比）③ `dec`（先看 `threads=`）
  ④ `update` ⑤ `present` ⑥ `sync`。

### 8.2 成本结构（量级，不是结论）

两类样本给出这条线的形状（跑满、无节拍）：

| 样本类型 | `本机`/帧 | 拆相形状 |
|---|---|---|
| **整屏变化**（每帧上千 tile，如看视频） | 几十 ms 量级 | **`decode` 占 9 成以上**（内部大半是 `dec` 的 tile 解码，其次 `update`），`zgx+parse`/`present` 各几个百分点 |
| **碎片**（命令多、矩形小但**总量不小**） | 十 ms 量级 | `decode` 约 6 成、`zgx+parse`/`present` 各约 2 成；**`sync` 可达帧墙钟的三分之一** |

- 跨样本比较必须先按**"每帧字节/命令数"归一**。（小矩形 + 小字节的轻样本是纯 CPU 受限；命令多且每帧
  脏区字节也大的样本上，帧率上限由 CPU 与那次脏区拷贝**共同**决定——压 `本机` 只买到能耗。）
- `dec` 之内（`prog2`，1/16 采样）的量级：`idwt` / `state` / `color` / `rlgr` / `upgrade` / `dequant`
  依次递减；**被默认当成"大头"的 RLGR 其实最小**。GPU 侧那条路"专治 RLGR"的账见
  [`gpu-accel-plan.md`](gpu-accel-plan.md) §4.4。
  - ⚠ **`idwt` 跑的是"抽取/外推"那一支，不是 `rfx_dwt_2d_decode_block`**：区域带
    `RFX_DWT_REDUCE_EXTRAPOLATE` 时解码器走 `rfx_dwt_2d_extrapolate_decode`（`progressive_rfx_idwt_x/_y`），
    全屏 Progressive 码流的每个区域都是这一支——对拍计数会显示 `codec/rfx_dwt.c` 的通用实现
    `rfx_dwt_2d_decode` **一次都没进**（`tiles=0`）。两支的算术也不同：抽取支用**截断除 2**（`(a+b)/2`）
    而不是算术右移，负数上两者不同，照抄时不能写成 `>>1`。抽取支的形状是
    `X2 = L − (H0+H1)/2`、`X1 = (X0+X2)/2 + 2*H0`、`X0 = X2`：**递推在内层**，且纵向那一趟原来按**列**走
    （每个样本换一条 cache line）——这两条是它比算术量更贵的原因。
  - **`dec` 的两种口径**（同一个"tile 解码段"，测点不同、不可混读）：并行时是**并行段的墙钟**（提交后到
     所有任务等完，`blocked` 是其中真正阻塞的部分）；串行时是**本线程逐 tile 解码的时间**（此时
     `dispatch`/`blocked` 必然为 0，不是缺失）。⇒ 读 `dec` 前先看 `threads=`。
  - **`update` 的大头本来是像素拷贝，不是记账**：`freerdp_image_copy_no_overlap` 把每个 tile（64×64×4B）
     拷进 surface；带 `KEEP_DST_ALPHA` 掩码（每像素一次掩码写）而不是 memcpy。region16 记账只值
     ~0.5ms/帧 量级（见 8.5）。**这份拷贝默认已经随 tile 解码段并行**（`update_tiles` 只留记账，见
     [`cpu-accel-plan.md`](cpu-accel-plan.md) §0/§2），所以并行下 `update` 只剩记账量级。
- **按字节算这笔账**（优化后期，单核的天平已经从"算力"倒向"每帧过多少字节"）：一个 tile（3 分量）大致
  搬 200KB 量级——RLGR 出 `sign`、去量化 `sign → buffer`、状态更新 `buffer↔current`、逆 DWT 三级级联、
  `color` 读系数写 tile、`update` 读 tile + 读目的（保 alpha）+ 写目的。各趟实测速率在
  **L1 级（几十 GB/s）到 DRAM 级（~10GB/s）**之间；**只有 `update` 明显低于 L1 速率**（它的目的按桌面
  行距跨行落，每行只用满一段）⇒ **再压就得压字节数**，把循环写窄没有用（见 8.5.3/8.5.4）。
- **compose**：`gdi_OutputUpdate` 的 surface→primary 合成；全屏会话里它已被"桌面镜像 surface 直接合成进
  primary"整段消掉（见 [`present-pipeline.md`](present-pipeline.md)）。
- **present**：绝大部分是**固定的 Vulkan 调用**（acquire / 录制 / submit / present），不是能删的活；
  `flush` 只有 µs 级。**GPU 侧另算**：`copy`（脏区字节 buffer→image）+ `blit`（clear 整张 + letterbox
  quad），**`blit` 与脏区无关**（见 [`present-pipeline.md`](present-pipeline.md) §2/§4.3）。**这正是零拷贝
  后端（Vulkan）与 GLES 的差别**：前者 gdi 直接合成进呈现器缓冲（`copy` 是唯一一次搬运），后者必须在
  CPU 侧把脏区 memcpy 进 staging 再上传 ⇒ 两个后端不构成"脏区形状"的对照实验。
  - ⚠ **零拷贝下 `copy` 与帧序是耦合的**：它读的就是本帧要写的那块内存，所以排在**本帧第一次写之前**
    （`sync`）。脏区字节大时它直接进帧墙钟。判据：**`sync` 与 `uploaded` 同步起落**就是这一项，
    而不是"合成贵"。
- **`setup`（非像素命令）**：低频但可能很重，要按**"每次调用分配多少字节"**查。典型暗礁是
  `ResetGraphics` 按桌面尺寸重分配大块 scratch（且会对多个 codec 集合各做一遍）；现在几何未变就直接返回。
  ⚠ 表面对比那一半（`CreateSurface` 的 `memset(surface->data, 0xFF)`）**不能省**：未绘制区域必须是 0xFF。
  ⚠ 也不要靠"关掉 planar codec"来省（`gdi_SurfaceCommand_Planar` 会拿到空上下文）。

### 8.3 量测纪律与陷阱

- **频率会跟着回放节拍跑**：轻负载样本被压在最低频档、重负载跑满，同一份代码能差数倍。
  ⇒ `mode=fast`（dev 页「跑满」）**不打节拍**；复现 live 的到达节奏用 `realtime`；
  **判读前先看 `cpuKHz=`，不同就不要横向比**。
- **只有同一次会话里的 A/B 才有效**：把待测特性关掉各跑几轮取中位数；只看 `(running=0)` 的整轮。
  用 QoS 去修频率**无效**（实测毫无变化）。跑满后轻样本会顶到显示/GPU 上限 ⇒ **A/B 纯 CPU 侧的改动
  要用 CPU 受限的样本**。
- **计时器本身会被测出来**：本平台 `clock_gettime` 不是 vDSO 级 ⇒ **per-tile 计时这条路放弃**；
  要量只能"单线程墙钟差"或**按比例采样**（`prog2` 就是 1/16）。`CLOCK_THREAD_CPUTIME_ID` 在本平台
  **不可信**（累计值可超过整轮进程 CPU）。
  - `prog2` 的读法：把**占比**与 `dec` 一起看——1/16 采样会让 `sum` 比 `dec` 高几个百分点（样本偏斜 +
    缩放），这点差不是发现了新开销。
- **探针只做一次性实验、量完就删**；长期保留的只有 per-message 计时（`read/dispatch/dec/update`、
  `calls/unions/tiles/tilesDec`）、**逐相位探针 `prog2`**、app 侧 `setup` 计数、present 的 GPU 时间戳、
  以及解码侧的**对拍计数**（8.4）。
- **不要用"跳过某条 dispatch/步骤 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
- **回放节拍若落在 `gdi_EndFrame` 里，必须扣掉**（否则算进 `compose`）：`GfxWorkMeter::OnPace` 为此存在；
  `sync` 同理，**别把"等 GPU"当"合成贵"**。
- **相加之和不等于整轮 `cpu=` 时要分清**：`cpu=` 是整轮进程 CPU（含 worker 线程、也含平台侧记账），
  相位只是 RDP 线程。**串行时两者应当几乎相等**——不等就先查探针，别先怀疑算力。并行时多出来的那部分
  就是池的账（可达解码段墙钟的十倍量级）——**不要用相位之和反推并行烧了多少电**，但可以用
  "`cpu=` − 串行那轮的 `cpu=`"估并行多烧的绝对量。
- **墙钟反推的"GPU 时间"不算 GPU 时间**：只有时间戳量到的才算，其余是排队。
  present 的 GPU 时间有长期探针（`vulkan present probe: gpu copy=… blit=…`）：**`copy` 正比于脏区字节、
  `blit` 与脏区无关**。
- **多轮测试要标准化，不要"猜"**：每个 stats 文本的**第一行**是
  `state=running|finished|aborted  run=<n>`，`native/scripts/replay-rounds.ps1` 就是按这个写的：
  - 点一次 → 等**这一轮**的 `run` 变成 `finished`。不要用"现在好像没在跑"来判断：两轮的 stats 文本
    长得一样，按内容猜必然把数据记到上一轮头上（`run=` 就是为此存在的）。
  - **一轮没跑完绝不点下一次**：dev 页的模式按钮（参考/节拍/路线/线程）内部都是 `stop + start`，
    中途点击 = 掐断，而**掐断的轮次不是测量**（驱动会直接把 `state=aborted` 报出来）。
  - 掐断的轮次**不写参考**（导出只在完整轮次落盘），否则一次半截导出会把好参考换成"跑了十几帧的哈希表"。
  - 脚本负责"设模式 → 推录像/参考 → 点重新回放 → 收 stats"，落到 `rounds-*.txt`；人只看文件。
- **dev 页驱动：别用盲点坐标**。先 `dumpLayout` 取控件 `bounds` 再点中心
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）；**按钮在顶栏（y 很小），而状态行会重复同名
  文字**（如 `路线:CPU`），按 `text` 找控件时要取顶栏那个。
  - ⚠ 驱动脚本用坐标点击，所以**窗口必须在最前**：应用被系统重启/窗口被切走时，点击会落到桌面上
    （症状：点了没反应，脚本会报"click did not start a new run"）。脚本因此先 `aa start` 把页面拉起来。

### 8.4 正确性门禁：参考画面 + 解码侧对拍

- **门禁 = 参考画面（golden reference）**，两份基线录像各一套：先 `参考:导出` 记下
  `hmrdp_ref_<captureTag>.{hash,bmp}`，之后每轮 `参考:对比` 报 `bad=0`。参考文件名带**录制内容指纹**，
  换一份录像必然缺参考、必须重新导出——这就是"同名文件的不同录制不能互相背书"的机器保证。
  - ⚠ **它是"输出不变"的门禁，不是"与旧解码器逐位一致"**：解码侧允许**舍入级**改动（`WITH_SIMD` 这类
    SIMD 实现不保证与通用 C 逐位相同），但**必须把差异量出来**：系数/像素最大偏差 **1 量级**、差异像素
    占比**千分之几** = 舍入，可以收；**成百上千** = 实现不同（16 位回绕、饱和当回绕、错索引），必须改回去。
    ⇒ **口径一变就要重录参考**，此后 `bad=0` 只表示"自那次重录起没有再变"（回归门），量级账由下一行承担。
  - ⚠ **参考证明不了"解码侧改动的量级"**（参考就是被改的那个解码器录的，两边一起变）：解码侧改动必须自带
    **对拍**——patch 里 `HmrdpDwtCheckStat[3]` = `{对拍过的 tile 数, 逐元素不同的个数, 最大 |Δ|}`，
    `参考:对比` 那一轮按 1/16 采样打开，stats 行报 `dwt check: tiles=… mismatch=… maxDelta=…`。
    - **判据是 `maxDelta`**：`0` = 逐位等价（最理想，参考可直接复用）；个位数 = 舍入；成百上千 = 实现不同。
    - 对拍要挂在**真正跑的那一支**上：`-DWITH_SIMD=ON` 时抽取支是 `codec/neon/rfx_neon.c` 的
      `rfx_dwt_2d_extrapolate_decode_neon`，逐位参照是 `HmrdpDwtExtrapolateReference()`（上游标量实现）；
      两边都要挂，否则会看到 `tiles=0` 这种"没对拍过"的假通过。
    - 取输入副本必须在**解码之前**：逆 DWT 的输出就写在系数缓冲上（4096 个系数全被覆盖），解码之后再复制
      拿到的是输出，两边算的不是同一件事。
    - **像素级量级**由 `参考:对比` 给：`rgbPx`（差异像素数）+ `maxDelta`（通道最大偏差）。两者都只看量级。
  - **逐像素诊断**（"差在哪一帧、差多少"）：`rgbPx/alphaPx/maxDelta/bbox` 只覆盖参考图像那一帧；要逐帧
    定位就重新 `参考:导出` 再比（引擎 vs gdi 的影子对比已从代码删掉，见 §6）。
  - 参考模式**不是性能模式**：每帧对整个桌面做一次哈希（`hashMs=`），这段时间落在 `EndFrame` 里、会算进
    `compose` ⇒ 读性能数字请用 `参考:关` 那一轮。
- **上游 NEON 的边界**：`codec/neon/rfx_neon.c` 的两支逆 DWT 都在**16 位车道上做加法**（`(a+b+1)>>1`
  与 `>>1`）——|a+b| 超过 int16 时会**回绕**，那不是舍入。实测本工程码流上从未触发（对拍 `maxDelta=1`
  恒成立），因此抽取支采用上游 NEON；**通用支仍用逐位等价实现**（它在 Progressive 码流上一次也不进）。
- **`bad=0` 覆盖不到的顺序约束**：上屏侧的帧序/等待（零拷贝单缓冲、picture ping-pong）不属于像素内容，
  判定只能靠机制 + 计数器（见 [`present-pipeline.md`](present-pipeline.md) §1）。

### 8.5 被否证的假设（成本该按什么估）

1. **"每次 tile 的 region16 union（O(n²)）是 `update` 的大头"** —— 先把相邻 tile 索引合并再 union，
   `unions ≈ tiles`（Progressive 每条消息的 region 基本就是每 tile 一个矩形，没有可合并对象）⇒ 零收益；
   再改成"每 tile 行一个 span、帧末一次并进 region"，`unions` 降了 50 倍以上而 `update` 只降 ~0.5ms
   ⇒ **union 总共只值 ~0.5ms**。⇒ 教训：**`update` 的账按"拷贝字节数"估，不要按"矩形条数"估**；
   而这份字节成本**落在哪条线程上**比它的多少更值钱（见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §2）。
2. **"delta 折叠（只拷没写过的区域）"** —— 去重 stamp 实测命中 **0 次**（帧内没有重复合成）⇒ 无收益。
3. **"`state` 的饱和加是算力瓶颈"** —— 那个循环按元素做两次分支钳位，看着像算力账；换成 `VQADD`
   （8 路、逐位等价的饱和加）后 `state` 在噪声内 ⇒ 它**受搬的字节数限制**（工作缓冲与持久 `current`
   两个缓冲都要写），不是受钳位限制。要降它得**少写一趟**，换 SIMD 不解决问题。
4. **"`update` 的 keep-alpha 拷贝是算力瓶颈"** —— 换成 4 像素/指令的字节掩码选择（逐位等价）后
   `update`/`本机` 都在噪声内 ⇒ 这个循环同样受字节数限制（16KB 读源 + 16KB 读目的 + 16KB 写目的，
   目的跨行落）。要动它只能动**字节数**（例如"目的 alpha 恒为 0xFF 时省掉那次读"，但那要求先证明该表面
   从不出现非 0xFF 的 alpha——`KEEP_DST_ALPHA` 是上游语义、alpha 也在参考哈希里，属**改语义**，
   不在"舍入"的许可范围内）。
   - ⚠ 该实验的**判据没当场证实**（`NEON_INTRINSICS_ENABLED` 依赖 CMake 生成的 `config.h`，
     "NEON 版真的编进去了"没有被独立验证）⇒ **这类实验要先证明"改动确实生效"**（打一个可观测计数），
     否则"中性"可能只是"没跑"。
   - ⇒ 教训：**"看着像算力"的循环，先用一个逐位等价的 SIMD 版本试一次**——它是中性的就说明账在内存侧，
     该动的是数据流而不是指令。

### 8.6 已定型的约束（decode 侧）

- **`update_tiles` 的记账形状已定型**：脏区按**每 tile 行一个 span** 记（帧末并进 region16），逐 tile 不再
  建 region16；这个形状本身只值 ~0.5ms/帧 量级，不要再去合并矩形或换 region 结构（两次实测收益都在噪声
  里，见 8.5.1）。**该段真正的成本是像素拷贝**，而它默认已随 tile 解码段并行
  （[`cpu-accel-plan.md`](cpu-accel-plan.md) §0/§2）。
- **`sign` / `current` 是两个持久状态**：`sign` 存 RLGR 原始系数、`current` 存去量化后的系数，
  两者跨消息常驻（UPGRADE 按 `sign` 判符号、DIFFERENCE 按 `current` 累加）⇒ 任何"少写一趟"的改动都要
  保证这两个缓冲最终内容不变（逐位等价），否则对拍会立刻显示出来。
- **上屏侧的约束**（脏区形状、零拷贝桌面缓冲、等待点、picture ping-pong）见
  [`present-pipeline.md`](present-pipeline.md) §1/§2/§4.4。