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

开关：全局「硬件解码」。关 / 无 Vulkan / 引擎初始化失败 ⇒ 回退 **gdi**。

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

### 2.3 帧呈现（gdi 回退路径）

- 按**脏区**部分上传/呈现：GLES 版靠 `glTexSubImage2D` + `GL_UNPACK_ROW_LENGTH`（= 整桌面 stride/4，
  否则按上传宽读行会花屏）；Vulkan 版对应 `vkCmdCopyBufferToImage` + `bufferRowLength`。
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
- **compute 派发注意并行度与访存形态**，不只是每轴工作组上限（`maxComputeWorkGroupCount`，常见 65535）：
  一个 workgroup 只覆盖 512~1536 个 invocation、且每个 invocation 串行处理 4096 个元素
  （RLGR 位流逐字节 refill、tile 逐像素循环）时，GPU 利用率极低——此时"引擎比 FreeRDP 的 CPU 软解还慢"
  是必然结果。整屏矩形需要 10 万+ 工作组，必须用 2D/3D 网格而不是线性下标。
- **任何带 `barrier()` 的 kernel，早退必须由整个 workgroup 一致决定**：`if (gid >= uNumStreams) return;`
  这类按 lane 早退会让最后一个 workgroup 的 `barrier()` 只被执行一部分 → **未定义行为**
  （实测表现为花屏 + present 失败）。要么在早退前先 `barrier()` 收敛，要么把参数补齐到整组。
- **`rfx_decode.comp` 是寄存器极度敏感的 kernel**：实测**只加两个计数器 + 一个 binding**就慢 2x。
  因此**不要在一个 kernel 里堆路径**（type 0/2/3 三套逻辑同文件），优先**按类型拆成多条 kernel**，
  每条只保留一条代码路径；改它的门槛指标是**每 stream 的 GPU 时间**（`gpuMs rlgr` / stream 数）。
- **不要用"跳过某条 dispatch + 差值反推"做归因**：跳过会改变后续数据相关的负载与依赖，实测偏差可达
  数倍，而且会渲染出花屏、容易被误判为回归。用 timestamp query 直接量（§6）。
- **不要用一小段"看起来固定"的耗时推断瓶颈**：先看它**是否随输入量变化**（本工程里 decode 的每 chunk
  耗时随码流字节在 0.26~48ms 之间变化），再下结论。
- **不要在非目标设备上标定性能**：真机口径要压的是**同步点数 / 驱动调用数 / CPU 介入次数**，
  不是模拟器耗时。
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
- 引擎屏幕经渲染器上屏（`GfxVkDesktop::Compose()` + `VkRenderer`）；GPU 接管时"本机解码耗时"计 0
  （解码已在 GPU），含义见 [`session-and-input.md`](session-and-input.md) 的遥测。

## 5. 历史包袱（**不要在新代码里依赖**）

- 旧的 **GLES 引擎 / EGL / GLES 渲染器**（`hmrdp_rfx.*` + `hmrdp_egl.*` + `hmrdp_renderer.*`）**已冻结**：
  它是 GLES 3.1 compute 版本，只作**算法与踩坑参考**，随清理删除。
  **它没有 §2.2 的那几条合成语义修复**，所以它的"对比路线"结果**不能当结论**。
- 已移除的 H.264/AVC 支持：它在真机上只有客户端广告 AVC444 时才被服务端启用（属微软非核心可选项），
  且命中硬解也无明显收益（瓶颈在解码后的 CPU 环节）⇒ 整体砍掉，服务端改用其他码流
  （Progressive / ClearCodec 等）。**不要再引入媒体库依赖**。

## 6. 验证回路（跨实现保留）

```
设置页「抓取 RFX 码流（测试）」  →  hmrdp_gfx.bin（原始 ZGX 字节，u32 长度 + payload）
dev 页「回放测试」五条路线：CPU / GLES / Vulkan / GLES对比 / Vulkan对比
```

- **验收口径**：`Vulkan对比` 路线（引擎 vs 离线 gdi 桌面逐像素比对）的
  `compare(GPU vs gdi): checks=… bad=0 rgbPx=0 maxDelta=0`。
  每 30 帧采一次（`kCompareEvery`），`bad` = 采样的帧里有多少帧与 gdi 不一致；目标 **`bad=0`**。
- 首次分歧会写一份 `<capture>.cmpdump`（逐像素 e/g 值），是定位分叉的第一手材料。
- **引擎性能归因三件套（dev，只在真机跑）**：
  1. `ProbeHostMemory`：逐个 host-visible 内存类型的写/连续拷贝/跨行拷贝带宽 → 决定 CPU 侧像素命令的成本（§1）；
  2. `ProbeSubmitCost`：空 command buffer 的 submit+fence 往返（实测 ~0.5ms）→ 用来区分"同步点固定开销"与
     "GPU 真的在跑"；没有它就会把 GPU 执行时间误判成同步开销；
3. **每条 dispatch 的 GPU 时间靠 timestamp query 直接量**（`rfx_decode`/`rfx_idwt`/`rfx_compose` 各一对
   `vkCmdWriteTimestamp`，fence 等待后 `vkGetQueryPoolResults` 读回，统计在引擎 `Stats()` 的 `gpuMs …` 行）。
   两端都取 `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`；**不要用"跳过某条 dispatch + 差值反推"**（见 §3）。
- 归因：需要"是哪条命令分叉"时，开 harness 的逐命令 A/B（见 [`build-and-verify.md`](build-and-verify.md)
  与源码里 `kCodecAbEnabled` 的注释）。**它是诊断工具**：开启后要按命令回读引擎表面，
  GLES 路线会慢到像卡死 ⇒ 只在对某条消息归因时开、查完立刻关（`bad=0` 的验收不依赖它）。
  **判定分叉只以 gdi 自己的表面为准**（历史上用手写镜像做过对比，它会误报）。
- **量测纪律（否则数字不可比）**：
  - 回放页的「路线 / 批次 / 重新回放」按钮内部都是 `stopReplayTest()` + 重新 `start`，**在一轮还没跑完时点击
    等于把那一轮掐断**；性能数字只取 `(running=0)` 的**整轮**，且要记下 `frames=` 以确认是整份跑完
    （不同录制的帧数不同，不能假定某个固定值）。
  - 判定"跑完"要**轮询** `replayTestStats` 文本里的 `(running=0)`，不要用固定 sleep：早取会拿到中途值，
    晚取白等。
  - `uitest dumpLayout` 的输出**不是合法 JSON**（部分字符串编码后 `ConvertFrom-Json` 会报错），
    用正则抓 `route=…(running=N)` 这类文本即可。
  - 性能探针（`ProbeHostMemory` / `ProbeSubmitCost`）是**进程内一次**（`RunDeviceProbes` 的 `call_once`）：
    每次切路线都会重建引擎，逐次重探只会拖慢启动并给数字加噪声。
- **采集内容与格式**：单文件 `hmrdp_gfx.bin`，存的是**服务端在 GFX 通道上、ZGFX 之前**的原始字节
  （每条 = `u32 长度` + 原始字节）；采集点在 `rdpgfx_on_data_received` 的 `zgfx_decompress` 之前，
  由 FreeRDP 补丁以运行期回调注册（见 [`native-libraries.md`](native-libraries.md) §3.7）。
  录制文件**不入库**：设备端在应用沙箱，本地副本放 gitignore 目录；dev 的抓取开关与「硬件解码」有联动
  （录制期间走软解），见设置页实现。
- **样本集（本地 `.cache/`，不入库）**：性能结论**必须分场景给**，且**每份捕获都要先自己过一遍
  `Vulkan对比 bad=0` 才能当基线**（同名文件的不同录制之间不能互相背书）：
  - `.cache/hmrdp_gfx.bin`：**浏览/滚动**（消息稀疏：多数帧 0~1 条消息，单条消息平均 ~430 条 stream）；
  - `.cache/hmrdp_gfx_video.bin`：**看视频**（整帧大块变化：多数帧 4~7 条消息，`diff`/restamp 占比高，
    每帧整屏脏 ⇒ 上屏接近整屏拷贝）。
  - 两场景的**瓶颈位置不同**：浏览场景在 GPU kernel；视频场景先被**上屏/提交结构**盖住
    （present 从 ~13ms/帧 涨到 >100ms/帧）。**判据要看"每 stream 的 GPU 时间"与当时在飞的 lane 数**，
    而不是看 dispatch 的总耗时——后者会随输入字节量变化（实测 0.26~48ms/chunk），
    所以"两个场景的平均值接近"并不说明大头是固定等待。
  - 单条 Progressive 消息最多可带**数千条 stream**（实测上限 4851 ⇒ ≈76 个 workgroup），
    这是"当前结构下"的并行度上限；平均值（数百）会低估它。
- **`Vulkan对比` 的既有残留**：视频录像目前 `bad=6`（整屏、maxDelta=255），**与本仓库的批量/解码改动无关**
  （关闭合并时同样 `bad=6`）。排查路径与工具见 [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。

- **差分测试（补齐捕获里没有的码流）**：统一方法 = **同一份载荷**分别喂 FreeRDP 解码器与我们的实现，
  逐像素比对。载荷优先用 FreeRDP 自带编码器生成；边界要覆盖**尺寸非 64 倍数**、纯色/渐变/UI 文本/alpha。
  ClearCodec 只有解码（运行时必须支持）没有编码器 ⇒ 只能用真实捕获；`CAPROGRESSIVE_V2` 双方都未实现 ⇒
  对齐"检测到即不支持"。
- 采集与回放需要**打过补丁并重编的 FreeRDP**（见 [`native-libraries.md`](native-libraries.md) §3.7）。

## 7. 待办

- **RLGR 解码 kernel 的并行化重设计**（producer/consumer，含已修/未解问题与实现要点）与
  **视频录像的正确性残留**：单独成文 → [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。
- **UI 收尾**：模拟器上置灰「硬件解码」与 GPU 回放入口（`DeviceCapabilities` 的 Capability 模式，
  给出原因），见 [`native-libraries.md`](native-libraries.md) §6。
- **删除冻结的 GLES 残留**（`hmrdp_rfx.*` 的引擎部分、`hmrdp_egl.*`、`hmrdp_renderer.*`）与仅服务于
  它的回放路线；共享的**容器解析器**（`ParseRfxProgressive`）与 GLES 无关，保留。
- **换样本复验**：不同分辨率（特别是宽/高为 **64 整数倍**的）、含**多条 REGION**消息的捕获。
