# CPU（gdi）链路优化计划

> **定位**：本文件是这条线的**接手文档**——判据、账目口径、已定型约束、优化清单与里程碑、
> 量测纪律**都在这里**，不要再去翻旧文档。协议/算法硬约束以 [`gfx-engine.md`](gfx-engine.md) §2 为准，
> 上屏以 [`present-pipeline.md`](present-pipeline.md) 为准，GPU 侧（另一条线）见
> [`gpu-accel-plan.md`](gpu-accel-plan.md)。
>
> **取代**：[`cpu-path_old.md`](cpu-path_old.md)（**old 弃用**，仅作历史依据：其中的成本数据与
> 被否证过程按原样保留）。章节号与旧文件保持一致，所以旧文件里的 `§N` 引用仍然指向同一主题。

**CPU 路线** = FreeRDP gdi 解码 + 我们自己的呈现器。

## 0. 优化判据（决定取舍，不是 fps）

**把每帧的计算成本降下来 = 能耗降下来**；fps 只要"够用"（跟得上服务端的到达节奏、不卡）。

- **判收益看**：每轮 `cpu=`（进程 CPU 时间）、`本机` 拆相、GPU/DRAM 流量。**不看**单纯墙钟。
  - ⚠ **`cpu=` 只在同一频率档下可比**：动态功耗 ≈ f·V²，多核把 SoC 压到低频档时，同一秒 CPU
    时间更便宜。所以"并行换了多少 CPU 秒"**不是能量结论**；要么在同一 `cpuKHz` 档下比，要么按
    "CPU 时间 × 档位"（或直接看 duty 下的功耗）估。
- **优先"少干活"**（去冗余拷贝、少唤醒、去掉重复搬运），但**不是因为"并行不好"**：
  多核低频在很多负载下比单核高频更省电，**并行本身是正当的能效手段**。不用它的原因是**现在这套并行
  不行**——它是 FreeRDP 自己的池（每 region 一次投递、每条消息唤醒 worker、一次 wait 抽干整池），
  没有针对本平台适配，效率低（§5），需要重写；而且混在单核优化里量不清楚。
  ⇒ **顺序：先做通用单核优化（§6 阶段一），再做多核与平台适配（§6 阶段二）**。
- **正确性口径：允许"舍入级"差异，但必须量出来**。`WITH_SIMD` 是开着的（FreeRDP 自己的 SSE/NEON
  实现：抽取支逆 DWT、量化移位、YCbCr→RGB、部分 primitives），它**不保证**与通用 C 逐位相同
  ⇒ 判据不是"逐位等价"，而是**量级 + 肉眼**：把差异量出来，确认是舍入（系数/像素最大偏差 1 量级、
  差异像素占比千分之几），而不是实现不同（回绕、错索引、饱和当回绕这类一次就给出成百上千的偏差）。
  量法见 §7（dev 对拍 `dwt check: … maxDelta=` + `参考:对比` 的 `rgbPx/maxDelta`）。
  **逐位等价的改写仍是首选**（差异为 0 时上面这笔账都不用记），但不再是唯一合法路径。
  - ⚠ 上游 `codec/neon/rfx_neon.c` 的两支逆 DWT 都在**16 位车道上做加法**：`(a+b+1)>>1` 与 `>>1`
    在 |a+b| 超过 int16 时会**回绕**——那不是舍入。实测本工程的码流上从未触发（对拍 `maxDelta=1`
    恒成立），因此抽取支采用上游 NEON；**通用支仍用逐位等价实现**（它在 Progressive 码流上一次也不进，
    换成上游 NEON 只赚到风险）。
  - ⚠ 口径一变，**参考画面必须重录**：`bad=0` 从"与旧解码器逐位一致"退化为"自那次重录起没有再变"
    （回归门），量级账交给对拍与 `rgbPx/maxDelta`。
- **现状：解码 worker 数固定为 1**（`kPinnedWorkers`）——这是一条**干净的单核基线**，正是阶段一要用的；
  旋钮与曲线见 §5，那是"打开旋钮时的知识"。

## 1. 账目与口径

CPU 回放的 stats 就是这条线的账：

| 行 | 含义 |
|---|---|
| `perFrame 本机=… = zgx+parse + decode + compose + present  (+ sync … blocked)` | **本机 = 处理时间**；各拆相相加**等于**本机，`sync` **不计入**（见下） |
| `prog ms/frame: read / dispatch / dec(blocked) / update  (calls= unions= tiles= tilesDec=)` | `decode` 的内部：`read` 读输入位流、`dispatch` 投递 tile（并行才有）、**`dec` = tile 解码段**、`update` = `update_tiles` 整段；`calls` 消息数、`unions`/`tiles` = `update_tiles` 的并集次数与被访问 tile 数、`tilesDec` = 真正解码的 tile 数 |
| `prog2 ms/frame (sampled 1/16, n=…): rlgr / dequant+diff / idwt / state / upgrade / color  sum=` | **`dec` 之内的拆相**（相位探针，见 §3）；`state` = 系数状态拷贝（`sign`/`current`）、`color` = `yCbCrToRGB` + 写 tile |
| `run … threads=… cpu=…s  cpuKHz=…` | 该轮的 worker 数与**整轮进程 CPU 时间**；`cpuKHz` 是本轮拿到的 SoC 频率档（见 §7） |
| `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp` | **非像素 GFX 命令**；它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 前先看这行** |
| `gfx setup: <Name> took … us` | 单条结构命令（模式切换/整面清零这类卡顿） |
| `uploaded/box/rectlist/truncated`、`present=` | 上屏侧：实际交给呈现器的字节、帧时间 |
| `ref export: frames=… -> hmrdp_ref_<tag>.{hash,bmp}  hashMs=…` / `ref compare: refFrames=… checks=… bad=… firstBad=… imageFrame=… rgbPx=… maxDelta=… bbox=… hashMs=…` | **参考画面对比**（golden reference，dev 页「参考」按钮，见 §7）：`bad` = 与参考逐帧哈希不同的帧数；`rgbPx/maxDelta/bbox` 只是参考图像那一帧的逐像素差 |

- **`sync` = 等 GPU 放开主缓冲**（`BeginDesktopBufferWrite`；CPU 路线让解码器直接写呈现器缓冲，
  所以**本帧第一次写之前**要等上一帧的 GPU 拷贝读完，等待点见 §4）。它是**阻塞**不是处理 ⇒ 单列；
  **帧的整段墙钟 = `本机` + `sync`**（回放再加节拍睡眠）。
  - ⚠ **这笔等待天然落在 `zgx+parse` 的窗口里**：帧首钩子（`StartFrame` / 帧内第一条表面命令）在
    `AccountChunkPrefix()` **之前**跑，而抓取的"一条记录"通常就是一整帧的 ZGX 段 ⇒ 该窗口覆盖了这次等待。
    所以测点必须把它交出来（`OnBlockedBeforeFrameWork()`，调用方用自己已经量到的那笔等待）：
    不交，`本机` 就把它算两遍、在 GPU 成为慢的一侧时虚高到 `sync` 那么多。
    自检：`本机 + sync ≈ 1/fps`——右边明显小于左边就是这里被算重了。
- **`pace` 是唯一直接剔除的项**：回放的人为节流不是客户端工作。
- **判读顺序**：① `setup` ② `kB/frame`/`cmds/frame`（内容是否可比）③ `dec`（先看 `threads=`）
  ④ `update` ⑤ `present` ⑥ `sync`。

## 2. 成本结构（量级，不是结论）

两类样本给出这条线的形状（跑满、无节拍）：

| 样本类型 | `本机`/帧 | 拆相形状 |
|---|---|---|
| **整屏变化**（每帧上千 tile，如看视频） | 几十 ms 量级 | **`decode` 占 9 成以上**（内部大半是 `dec` 的 tile 解码，其次 `update`），`zgx+parse`/`present` 各几个百分点，`sync` 与 `present` 同量级 |
| **碎片**（命令多、矩形小但**总量不小**） | 十 ms 量级 | `decode` 约 6 成、`zgx+parse`/`present` 各约 2 成；**`sync` 可达帧墙钟的三分之一** |

⇒ 整屏样本里 **decode 占绝对多数**：任何"上屏/合成"侧的优化都不可能显著改变这条线。
碎片样本再分两种：**小矩形 + 小字节**的轻样本是 CPU 受限；**命令多、每帧脏区字节也大**的样本上，
GPU 那次脏区拷贝（`sync`）能占到帧墙钟三分之一量级 ⇒ 帧率上限由 CPU 与这次拷贝**共同**决定，
此时压 `本机` 只买到能耗，要动帧率得动拷贝的量（见 §3/§4）。
本章其余各节的结论都按"每帧字节/命令数"归一后才可跨样本比较（见 §7）。

## 3. 一帧的钱花在哪

- **decode**：内部拆成 `read`（很小）+ `dispatch`（投递开销）+ `dec`（**tile 解码段**）+ `update`。
  其中 `dec` 的内部（`prog2` 行，1/16 采样）实测形状——**两个样本一致**：

  | `dec` 之内 | 占比量级 | 是什么 |
  |---|---|---|
  | `idwt` | **4 成半** | 逆 DWT：每个分量 3 级 lifting，标量 int16，逐行/逐列各一遍，另有一块临时缓冲。**跑的是"抽取/外推"那一支**，见下 |
  | `state` | 约 2 成 | 系数状态拷贝：`sign`（RLGR 原始系数）+ `current`（去量化后），每 tile 约 50KB 的读写 |
  | `color` | 约 1 成半 | `yCbCrToRGB_16s8u_P3AC4R`（整数乘加 + 逐像素 `writePixelBGRX`） |
  | `rlgr` | 约 1 成 | RLGR 位解码（三个分量各 4096 系数） |
  | `upgrade` | 半成 | type 2/3 的逐系数细化 |
  | `dequant+diff` | 百分之几 | 10 段 `lShiftC` + LL3 差分 |

  - ⚠ **`idwt` 那一支是"抽取"变体，不是 `rfx_dwt_2d_decode_block`**：区域带 `RFX_DWT_REDUCE_EXTRAPOLATE`
    时解码器走 `rfx_dwt_2d_extrapolate_decode`（`progressive_rfx_idwt_x/_y`），而全屏 Progressive 码流
    的每个区域都是这一支——dev 对拍计数会显示 `codec/rfx_dwt.c` 的通用实现 `rfx_dwt_2d_decode`
    **一次都没进**（`tiles=0`）。两支不只是访存形状不同：抽取支用**截断除 2**（`(a + b) / 2`）
    而不是算术右移，负数上两者结果不同，照抄时不能写成 `>>1`。
  - 抽取支的形状：`X2 = L - (H0 + H1)/2`、`X1 = (X0 + X2)/2 + 2*H0`、`X0 = X2`——递推在**内层**，
    且 `idwt_y` 原来按**列**走（每个样本换一条 cache line），这两条是它比算术量更贵的原因。

  ⇒ **被默认当成"大头"的 RLGR 其实最小**（1 成量级）；真正的钱在**逆 DWT**与**逐 tile 缓冲流量**
  （`state` + `color`）。GPU 侧那位"专治 RLGR"的现状见 [`gpu-accel-plan.md`](gpu-accel-plan.md) §2/§4.4。
  - **`dec` 的两种口径**（同一个"tile 解码段"，测点不同、不可混读）：并行时它是**池段的墙钟**
    （RDP 线程投递后到所有 work 全部等完，`blocked` 是其中真正阻塞的那部分——两者只在并行时有值）；
    串行时它是**本线程逐 tile 解码的时间**（此时 `dispatch`/`blocked` 必然为 0，不是缺失）。
    ⇒ 读 `dec` 前先看 `threads=`：串行那轮 `read + dec + update ≈ decode`（缺口只有消息级开销）。
  - **`update` 的大头是像素拷贝，不是记账**：`freerdp_image_copy_no_overlap` 把每个 tile
    （64×64×4B）拷进 surface，整帧字节量在 MB 量级；因为带 `KEEP_DST_ALPHA` 掩码（每像素一次掩码写）
    而不是 memcpy，实测带宽只有 **~1GB/s** 量级。region16 记账只占 **~0.5ms/帧** 量级（见 §8）。
- **compose**：`gdi_OutputUpdate` 的 surface→primary 合成；全屏会话里它已被
  "桌面镜像 surface 直接合成进 primary"整段消掉（§4）。
- **present**：绝大部分是**固定的 Vulkan 调用**（acquire / 录制 / submit / present），不是我们能删的活；
  `flush`（把本帧写过的字节交出去）只有 µs 级。**GPU 侧另算**：`copy`（脏区字节 buffer→image）+
  `blit`（clear 整张 + letterbox quad），**blit 与脏区无关**（ms 量级，见
  [`present-pipeline.md`](present-pipeline.md) §2/§4.3）。**这正是零拷贝后端（Vulkan）与 GLES 的差别**：
  前者 gdi 直接合成进呈现器缓冲（`copy` 是唯一一次搬运），后者必须在 CPU 侧把脏区 memcpy 进 staging
  再上传 ⇒ 两个后端不构成"脏区形状"的对照实验。
  - ⚠ **零拷贝下 `copy` 与帧序是耦合的**：它读的就是本帧要写的那块内存，所以排在**本帧第一次写之前**
    （`sync`）。脏区字节大时它直接进帧墙钟。判据：**`sync` 与 `uploaded` 同步起落**就是这一项，
    而不是"合成贵"。
- **`setup`（非像素命令）**：低频但可能很重，要按**"每次调用分配多少字节"**查。典型暗礁是
  `ResetGraphics` 按桌面尺寸重分配大块 scratch（且会对多个 codec 集合各做一遍）；现在几何未变就直接返回。
  ⚠ 表面对比那一半（`CreateSurface` 的 `memset(surface->data, 0xFF)`）**不能省**：未绘制区域必须是 0xFF。
  ⚠ 也不要靠"关掉 planar codec"来省（`gdi_SurfaceCommand_Planar` 会拿到空上下文）。

## 4. 已定型的几件事（不要再动，连同约束）

- **脏区形状**：staging 路径用**逐条矩形**（成本在字节，box 会多搬数倍）；零拷贝路径用**合并 box**
  （CPU 侧成本在条数：每条 = 一个 copy region + 一次 flush，而 box 恒一条；已按 CPU 侧 `present` 量过：
  box ≤ rect list，碎片样本 −17%）。由 `FramePresenter::usesDesktopBuffer()` **分流**，
  ⇒ 它不是可独立调的旋钮：GLES（交不出主缓冲）恒走矩形、Vulkan（零拷贝）恒走 box，
  「把碎矩形并成长条」这条**作废**。box 的字节量是 GPU 那次 buffer→image 拷贝的量、并排在帧首
  （`sync`，§3）——那是"等 GPU"的账，不是形状选错了；跨后端比 `present`/`sync` 更不是 box/rect 的 A/B。
- **零拷贝上屏：gdi 主缓冲 = 呈现器的 host-visible 缓冲**（`gdi_init_ex` + 呈现器自带缓冲）。
  实现约束（都要遵守）：
  - 呈现器**每帧问一次、成功为止**（Vulkan 设备随 surface 建，`gdi_init` 时可能还没有），
    live 与回放共用 `InitGdiWithPresenter` / `AttachPresenterDesktopBuffer`。
  - **挂接不要用 `gdi_resize_ex`**（它会再调 `update_end_paint`，把 `update->mux` 的配对搞乱）；
    就地换 `primary->bitmap` 的 data/scanline/free 与 `gdi->stride`，**换前把已合成的桌面拷过去**
    （gdi 图元会读目标缓冲）。
  - **单缓冲 ⇒ 本帧第一次写之前必须等上一帧的 GPU 读完**（§1 的 `sync`）。这条是**顺序约束**：
    等待点取在 `update->BeginPaint` 是错的（那是 `gdi_OutputUpdate` 里、整帧解码之后发的），
    正确位置是**每个可能写桌面的命令之前**（GFX 帧边界与表面命令，以及 SolidFill / SurfaceToSurface /
    缓存 / ResetGraphics 这类结构命令——**帧外**同样会来表面命令），实现挂在 `GfxWorkSetFrameBeginHook`。
    等待按"提交"去重（`desktopBufferFencePending_`）⇒ **每个 present 只真等一次**。
    ⚠ 等待缺失的后果是**上屏画面混两帧**，且**只在帧背靠背时**出现；`bad=0` 与它无关。
  - 内存类型 `HOST_CACHED` 优先；非连贯时要 flush，**按脏区合并成一个区间刷**（逐条刷会变成每帧上百次
    驱动调用；cache flush 只写回脏行，多出来的干净行免费）。
  - 几何变化（`ResetGraphics` → `gdi_resize`）会退回 gdi 自有缓冲，下一次 `EndPaint` 按新尺寸重挂。
  - 前提：**进程内只建一个 `VkDevice`**（见 [`gfx-engine.md`](gfx-engine.md) §1）。
- **桌面镜像 surface 直接合成进 primary**（全屏 GFX 会话的常态：只有一个 surface、`(0,0)` 1:1、
  格式/行距与桌面相同 ⇒ 它**就是桌面**，那趟逐矩形 `freerdp_image_scale` 纯属白搬）。
  做法是把这个 surface 的 `data` 指向 `gdi->primary_buffer`。**生命周期是重点**：
  - 凭证就是 `surface->data == gdi->primary_buffer`；任何一条不满足就退回逐矩形拷贝。
  - `gdi_ResetGraphics` **保留** surface 并 memset 它，而它调的 `DesktopResize` 会**换掉 primary**
    ⇒ 必须**换之前**记下谁在共享、换之后重指向或让它自己分配。
  - `gdi_DeleteSurface` 不能释放共享缓冲；出现**第二个** surface 时先解除共享。
  - ⚠ **gfx 上下文拆卸也要经 `DeleteSurface` 走一遍**（`rdpgfx_client_context_free` → `free_surfaces`），
    而 `gdi_DeleteSurface` 从 `context->custom` 取 gdi —— `gdi_graphics_pipeline_uninit` 已经把它清空
    ⇒ 共享测试必然失败、把**呈现器的 host-visible 缓冲**当自有缓冲 `winpr_aligned_free`（进程直接挂）。
    **顺序**：谁拆 gfx 上下文，谁就必须**在 uninit 之前**先 `DeleteSurface` 掉所有 surface
    （离线 CPU 桌面就是这么做）；live 靠 FreeRDP 在 `OnClose` 里先 `free_surfaces`。
  - 全在 `libfreerdp/gdi/gfx.c` 内部，**不动头文件也不动 app**，因此对任何呈现器都成立。
- **`update_tiles` 的 region16 记账不是瓶颈**（~0.5ms/帧 量级）：不要再去合并矩形/换 region 结构
  （两次实测收益都在噪声里，见 §8）。

## 5. 并行解码：能效曲线与结论

唯一可用的并行度旋钮是 **tile 解码的 worker 数**（WinPR 线程池，每个 codec 一个池；本平台**不能绑核/绑簇**）。
控制面：patch 导出的 `HmrdpSetDecodeThreads/Get/Apply`，app 侧 `hmrdp_decode_tuning.*`，设置页「解码线程数」
（0=自动 / 1..8），dev 回放页有「线程」；**`n == 1` 走完全串行分支**（不建池、不提交、不唤醒、不等待）。
自动值 = `min(性能核数或核数, 4)`。

> **现状：worker 数固定为 1（`hmrdp_decode_tuning.cpp` 的 `kPinnedWorkers`）**。并行那头是 FreeRDP 自己的
> 池（每 region 一次投递、每条消息唤醒 worker），没有针对本平台优化、效率低，且预期会重写；在重写之前
> 统一用串行，测量里就少一个自由变量。设置页的「解码线程数」与回放页的「线程」因此**置灰**（存的值保留，
> 重新开放只需把 `kPinnedWorkers` 改回 0 并去掉两处置灰）。下面这张曲线是**打开旋钮时的知识**。

标定结果（同一样本内比较）：

| worker | 墙钟（`本机`/帧） | 整轮进程 CPU |
|---|---|---|
| 1（串行） | 整屏样本 2× 出头（fps 约 0.5×）；碎片样本接近（+4% 量级） | **最低**（整屏约 35%，碎片约 45%） |
| 2 | 已到并行平台 | 平台值 |
| 4（自动） | 与 2/6/8 相同 | 平台值 |
| 6 / 8 | 不再下降 | 不再变化 |

- **≥2 之后墙钟完全不涨**：串行→2 的跳变已超过 2x，2→8 又不涨 ⇒ **每个 tile 的有效成本随并发度变化**，
  不是"把工作平均切开"。机制是**并发下的访存/缓存干扰** ⇒ **调 worker 数是把问题换个位置暴露，不是解**。
- **并行买到的墙钟远少于核数**：整屏样本 4 个 worker 只把 tile 解码段压到串行的三分之一量级（`dec`），
  而"解码之外的线程 CPU"是那段墙钟的**十倍量级** ⇒ 池里大部分 CPU 花在争用/空转（chunked 投递的
  共享计数自旋）而不是解码。**这是"现在这套池"的问题，不是"并行"的问题**——重写池 / 换平台任务队列
  （§6 阶段二）才是对的方向。
- ⚠ **两轮的频率档不同，别把 CPU 秒数直接当能量**：多核一起跑时 SoC 会把档压低，单核那轮能拿到的
  最高档明显更高（§7）⇒ 上面那两轮的 `cpu=` 不能横向读成"并行多烧了 N 倍电"；
  要下能量结论就在同一 `cpuKHz` 档下比，或按"CPU 时间 × 档位"估。
- **串行确实更省，但省多少要按档位读**：整屏样本串行慢 2× 出头、`cpu=` 只有并行的 35%；碎片样本
  墙钟几乎不变（+4% 量级）而 `cpu=` 只有 45%。⚠ 这些比例是**跨频率档**量的（§0 的提醒），
  只能当"同一套池下、同一样本内"的相对关系，不能当"并行 N 倍电"的结论。
- 已否定：池 fan-out 4→8（`dec`/`本机` 都没有收益）——**在现有池实现下**已到头；换池/换任务队列
  要重开这条打分（§6 阶段二）。

## 6. 优化清单与里程碑

**总顺序：阶段一（通用单核优化）→ 阶段二（多核与平台适配）。** 理由：
① 单核几项是**并行路径的乘数**（每个 tile 的算法成本降下来后，n 个 worker 的总成本同比例下降，
并行调优的起点也更高）；② 并行那头的池/调度是**独立的一件事**，和算法优化混在一起量不清
（调试变量太多）；③ 现在 worker 数固定为 1（§5），正好有一条**干净的单核基线**可用。

**阶段一的排序依据**是 `prog2` 的实测占比（§3）：`dec` 之内 `idwt` 约 4 成半、`state`+`color` 共约 3 成半、
`rlgr` 只 1 成 ⇒ **不要从 RLGR 入手**（它最小；GPU 侧那条路另有账，见
[`gpu-accel-plan.md`](gpu-accel-plan.md) §4.4）。

### 阶段一：通用单核优化（现在的目标）

| # | 项 | 占比 | 做法要点 | 正确性门禁 | 出口 |
|---|---|---|---|---|---|
| **C1** | 逆 DWT 改写（**做完了**） | 4 成半 | 抽取支（`progressive_rfx_idwt_x/_y`，Progressive 实际跑的那一支）不动算术、只改组织：① `X2` 序列（`L - (H0+H1)/2`）**先整段算出来**，输出对再由一层无依赖的循环写；② `idwt_y` 改成**行主序**（原来按列走，每个样本换一条 cache line）；③ scratch 只有一条行级缓冲。`codec/rfx_dwt.c` 的通用支另做了逐位等价的 NEON 改写（`VRHADD` = `(a+b+1)>>1`、`VHADD` = `(a+b)>>1` 都是全精度和） | **① 标量 vs 新实现采样对拍**（`mismatch=0`）+ **② `参考:对比` `bad=0`**（见 §7） | `idwt` 绝对量 **−2 成**、`decode`/`本机` 各 **−5%**、整轮 `cpu=` **−4%**（两份录像都过门禁） |
| **C1b** | 打开 FreeRDP 自己的 SIMD（**做完了**） | 同上 | `-DWITH_SIMD=ON`：抽到抽取支逆 DWT（8 路 16 位）、量化移位、`yCbCrToRGB` 与部分 primitives 的 NEON 版；非抽取支的 NEON 不接（见 §0） | 不是"逐位"，而是**量级门禁**：对拍 `maxDelta=1`（约 2% 系数差 1）+ `参考:对比` 的 `rgbPx` 占屏 0.5%/0.2%、`maxDelta=1`；之后重录参考并复核 `bad=0` | `idwt` **−65%**、`color` **−57%**、`decode` **−35%**、`本机` **−30%**、`cpu=` **−29%**、`fps` **+42%** |
| **C2** | `state` + `color` 的逐 tile 缓冲流量 | 共 3 成半 | ① 三次状态写并成一次遍历（或让 RLGR 直写 `sign`、去量化直写 `current`，省掉中间那份）；② `yCbCrToRGB` 的逐像素 `writePixelBGRX` + 整数乘加向量化（同一批系数与移位） | 同上（对拍 + 参考对比） | 同上（②已被 C1b 拿掉大半，剩下的账在 ①） |
| **C3** | `update` 的 tile→surface 拷贝 | 1 成半（串行） | keep-dst-alpha 的"每像素一个 32 位掩码字"→**每两像素一个 64 位掩码字**（逐位等价）+ 自动向量化 | `参考:对比` `bad=0`（同样要求逐位等价） | `perFrame decode` |
| — | `upgrade` / `dequant` | 半成 / 百分之几 | 占比小，C1–C3 之后再评估 | — | — |
| — | RLGR | 1 成 | 最小项，最后再看 | — | — |

- **C1/C1b 之后账目的形状变了**：整屏样本 `decode` 从"绝对多数"降到 **`dec` 的七成上下仍是 DWT +
  `state`**，而**碎片样本的上限换到了 `present` 侧**——`本机` 压下去以后，每帧墙钟由那一趟脏区
  拷贝（`sync`，可达 `本机` 的七成）决定，这正是 §3/§4 说的"要动帧率得动拷贝的量"。
- **C1 与 C1b 的分工**：C1 是**逐位等价**的改写（差异 0，任何构建下都成立）；C1b 是**允许舍入**的那类
  改动。两者叠加才有上表的效果，但只有 C1 能无条件重跑参考对比，C1b 必须连带重录参考（§0）。
- **剩下的余地**：抽取支的 DWT 已是 8 路 16 位，再压只能动算法结构（而不是指令宽度）；`state`
  那三趟 8KB 的搬运（§6 C2①）现在是 `dec` 里最大的一块。

- **C1 的现状与它剩下的余地**：改写后 `idwt` 仍占 `dec` 的四成上下，绝对量的下降来自"组织"
  （去掉列向走位、把递推挪出内层），**不是**来自位宽更小的指令——抽取支分母是**截断除 2**，
  16 位车道里没有现成指令（`VHADD` 是 floor），要再往下压只能与"负数奇数才 +1"的修正项做对比
  （或用 32 位车道，收益约再一倍）。先把 C2/C3 做完再回来评估。
- 三项都属"**少搬字节 / 同一批算术换组织**"，不是"把活干快"（§0）；向量化只允许**逐位等价**的改写。
- **前置对齐**：C2 里"让 RLGR 直写 `sign`/`current`"会动寄存的布局，和 GPU 阶段化（
  [`gpu-accel-plan.md`](gpu-accel-plan.md) §4.3）是同一块地，动手前先跟那边对齐，别为 CPU 优化把阶段化挡住。
- 出口一律用**同一频率档**（或 `realtime` 的同一节拍）下的 A/B；跨档只报相对关系（§0/§7）。

### 阶段二：多核与平台适配（阶段一之后）

**目标**：把"多核低频"这条能效路线做对——它本身是正当手段（§0），要做的是**换掉现在这套并行**，
而不是回避并行。

| # | 项 | 做法要点 | 门禁/出口 |
|---|---|---|---|
| **M-a** | **换池/换任务队列**：`ffrt`（提交任务、由系统决定何时/在哪跑）或等价的平台队列，按 [`native-libraries.md`](native-libraries.md) §6 的 Capability 模式探测 + 降级；保留 WinPR 池作兜底 | 不能碰：**同一个 tile 仍由单个 callback 独占解码**（否则参考对比失去意义）；出口：同一档下的 `dec`/`本机`/`cpu=` |
| **M-b** | **producer/consumer 流水线**：worker 解码 region k 时，RDP 线程解析 k+1 / 合成 k−1，把现在"每条 region 一次 fork/join + 解析/`update` 全在 RDP 线程"的串行段藏起来 | ⚠ 合成的顺序语义（同帧重复合成、clip 取 REGION 头）见 [`gfx-engine.md`](gfx-engine.md) §2.2；出口同上 |
| **M-c** | **worker 数按 duty 自适应**：到达侧限速（duty 低）时回落 1，受限时用 2–4；判据用已有的到达间隔/duty 加滞回，旋钮与热切换早就有 | 出口：同一录像的 duty↔worker 曲线；`cpu=` 按档位换算 |

- **入口条件**：阶段一的 C1–C3 落地并有基线（否则"并行快了"分不清是算法还是调度）；
  并且 M-a 之前**不要**再调 `kPinnedWorkers`/fan-out——那只是把同一个问题换个位置暴露（§5）。
- **判据补充**：这一阶段的收益要按"**同档 CPU 时间**"或"duty 下的功耗"读；多核与单核跑在不同频率档
  是常态，`cpu=` 秒数不能直接比（§0）。

**待收尾**：patch 第 14 步（`hmrdpDirtyLeft/Right`：脏区记成"每 tile 行一个 span"而不是逐 tile union）
当初量到**净收益 ~0**；它现在仍在树里，而近几轮参考对比是 `bad=0`。两条路选一条并重跑两份录像确认：
撤掉（回到逐 tile union 的简单基线），或保留（并把 §4 的那条结论改成"span 形状已定型"）。

## 7. 量测纪律与陷阱

- **正确性门禁 = 参考画面（golden reference）**，两份基线录像各一套：先 `参考:导出` 记下
  `hmrdp_ref_<captureTag>.{hash,bmp}`，之后每轮 `参考:对比` 必须 `bad=0`（逐帧哈希与参考一致）。
  参考文件名带**录制内容指纹**，所以换一份录像必然缺参考、必须重新导出——这就是"同名文件的不同录制
  不能互相背书"的机器保证。
  - ⚠ **它要求"逐位等价"**：参考记录的是 gdi 在被优化之前的输出，所以**只接受逐位等价的改写**
    （这正是 §0 的硬约束）。改了解码语义（哪怕只是舍入不同）就必须**重新导出参考**，并说明为什么可以接受。
  - ⚠ **参考证明不了"解码侧改动的量级"**：参考就是被改的那个解码器录的，两边一起变。解码侧的改动必须
    自带**对拍**：patch 里 `HmrdpDwtCheckStat[3]` = `{对拍过的 tile 数, 逐元素不同的个数, 最大 |Δ|}`，
    `参考:对比` 那一轮开着（1/16 采样），stats 行报 `dwt check: tiles=… mismatch=… maxDelta=…`。
    - **判据是 `maxDelta`**：`0` = 逐位等价（最理想）；**个位数** = 舍入，可以收（§0）；
      **成百上千** = 实现不同（16 位回绕、饱和当回绕、错索引），必须改回去。
    - 对拍要挂在**真正跑的那一支**上：`-DWITH_SIMD=ON` 时抽取支是 `codec/neon/rfx_neon.c` 的
      `rfx_dwt_2d_extrapolate_decode_neon`，逐位参照是 `HmrdpDwtExtrapolateReference()`（progressive.c
      里的上游标量实现）；两边都挂，否则会看到 `tiles=0` 这种"没对拍过"的假通过。
    - 取输入副本必须**在解码之前**：逆 DWT 的输出就写在系数缓冲上（4096 个系数全被覆盖），
      解码之后再复制 `buffer` 拿到的是输出，两边算的不是同一件事。
    - **像素级量级**由 `参考:对比` 给：`rgbPx`（差异像素数）+ `maxDelta`（通道最大偏差）。两者都只看
      量级（占屏比例、最大偏差），不记单次流水。
  - **逐像素诊断**（"差在哪一帧、差多少"）：参考对比里的 `rgbPx/alphaPx/maxDelta/bbox` 只覆盖参考图像那一帧；
    要逐帧定位就重新 `参考:导出` 再比（引擎 vs gdi 的影子对比已从代码删掉，见 `gfx-engine.md` §6）。
  - 参考模式**不是性能模式**：每帧对整个桌面哈希（`hashMs=` 报出来），这段时间落在 `EndFrame` 里、
    会算进 `compose` ⇒ 读性能数字请用 `参考:关` 那一轮。
- **频率会跟着回放节拍跑**：轻负载样本被压在最低频档、重负载跑满，同一份代码能差数倍。
  ⇒ `mode=fast`（dev 页「跑满」）**不打节拍**；复现 live 的到达节奏用 `realtime`；
  **判读前先看 `cpuKHz=`，不同就不要横向比**。
  - **`cpu=` 的读法**：它是"能耗代理"，但只在**同一档**成立（动态功耗 ≈ f·V²）。多核/少核、
    跑满/限速这两类对比天然落在不同档 ⇒ 要么在同档下 A/B，要么按"CPU 时间 × 档位"换算，
    要么看 duty 下的实际功耗；**不要拿两轮的 CPU 秒数直接相除当能量比**。
  - **只有同一次会话里的 A/B 才有效**：把待测特性关掉各跑几轮取中位数；只看 `(running=0)` 的整轮。
  - 用 QoS 去修频率**无效**（实测毫无变化）。
  - 跑满后轻样本会顶到显示/GPU 上限 ⇒ **A/B 纯 CPU 侧的改动要用 CPU 受限的样本**。
- **计时器本身会被测出来**：本平台 `clock_gettime` 不是 vDSO 级 ⇒ **per-tile 计时这条路放弃**；
  要量只能"单线程墙钟差"或**按比例采样**（`prog2` 就是 1/16）。
  `CLOCK_THREAD_CPUTIME_ID` 在本平台**不可信**（累计值可超过整轮进程 CPU）。
- **探针只做一次性实验、量完就删**；长期保留的只有 per-message 计时（`read/dispatch/dec/update`、
  `calls/unions/tiles/tilesDec`）、**逐相位探针 `prog2`**（`rlgr/dequant/idwt/state/upgrade/color`）、
  app 侧 `setup` 计数和 present 的 GPU 时间戳。
  - `prog2` 的读法：把**占比**与 `dec` 一起看——1/16 采样会让 `sum` 比 `dec` 高几个百分点（样本偏斜 + 缩放），
    这点差不是发现了新开销。
- **不要用"跳过某条 dispatch/步骤 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
- **回放节拍若落在 `gdi_EndFrame` 里，必须扣掉**（否则算进 `compose`）：`GfxWorkMeter::OnPace` 为此存在；
  `sync` 同理（见 §1），**别把"等 GPU"当"合成贵"**。
- **相位是"同一线程上的不重叠区间"**，所以要先过一遍 `本机 + sync ≈ 1/fps`：左边明显大于右边，
  说明有一笔等待被算了两次（§1），此时 `本机` 不是算力。
- **相加之和不等于整轮 `cpu=` 时要分清**：`cpu=` 是整轮进程 CPU（含 worker 线程、也含平台侧记账），
  相位只是 RDP 线程。**串行时两者应当几乎相等**——不等就先查探针，别先怀疑算力。
  并行时多出来的那部分就是池的账（§5：可达解码段墙钟的十倍量级）——**不要用相位之和反推并行烧了多少电**，
  但可以用"`cpu=` − 串行那轮的 `cpu=`"估并行多烧的绝对量。
- **墙钟反推的"GPU 时间"不算 GPU 时间**：只有时间戳量到的才算，其余是排队。
  present 的 GPU 时间有长期探针（`vulkan present probe: gpu copy=… blit=…`）：**`copy` 正比于脏区字节、
  `blit` 与脏区无关**。
- **多轮测试要标准化，不要"猜"**：每个 stats 文本的**第一行**是
  `state=running|finished|aborted  run=<n>`，`native/scripts/replay-rounds.ps1` 就是按这个写的：
  - 点一次 → 等**这一轮**的 `run` 变成 `finished`。不要用"现在好像没在跑"来判断：两轮的 stats 文本
    长得一样，按内容猜必然把数据记到上一轮头上（`run=` 就是为此存在的）。
  - **一轮没跑完绝不点下一次**：dev 页的模式按钮（参考/节拍/路线/线程）内部都是 `stop + start`，
    中途点击 = 掐断，而**掐断的轮次不是测量**（驱动会直接把 `state=aborted` 报出来）。
  - 掐断的轮次**不写参考**（导出只在完整轮次落盘），否则一次半截导出会把好参考换成"跑了十几帧的
    哈希表"。
  - 脚本负责"设模式 → 推录像/参考 → 点重新回放 → 收 stats"，落到 `rounds-*.txt`；人只看文件。
- **dev 页驱动：别用盲点坐标**。先 `dumpLayout` 取控件 `bounds` 再点中心
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）；**按钮在顶栏（y 很小），而状态行会重复同名文字**
  （如 `路线:CPU`），按 `text` 找控件时要取顶栏那个。另外「路线/重新回放/线程/加速」内部都是 `stop + start`，
  **一轮没跑完时点击 = 掐断**。
  - ⚠ 驱动脚本用坐标点击，所以**窗口必须在最前**：应用被系统重启/窗口被切走时，点击会落到桌面上
    （症状：点了没反应，脚本会报"click did not start a new run"）。脚本因此先 `aa start` 把页面拉起来。

## 8. 两个被否证的假设（成本该按什么估）

1. **"每次 tile 的 region16 union（O(n²)）是 `update` 的大头"** —— 修正过程：
   先把相邻 tile 索引合并再 union，`unions ≈ tiles`（说明 Progressive 每条消息的 region **基本就是
   每 tile 一个矩形**，没有可合并对象）⇒ 零收益；再改成"每 tile 行一个 span、帧末一次并进 region"，
   `unions` 降了 50 倍以上，而 `update` 只降 ~0.5ms ⇒ **union 总共只值 ~0.5ms**。
   ⇒ 教训：**`update` 的账要按"拷贝字节数"估，不要按"矩形条数"估**。
2. **"delta 折叠（只拷没写过的区域）"** —— 去重 stamp 实测命中 **0 次**（帧内没有重复合成）⇒ 无收益。
