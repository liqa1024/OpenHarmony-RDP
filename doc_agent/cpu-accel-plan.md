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

- **判收益看**：每轮 `cpu=`（进程 CPU 时间，能耗代理）、`本机` 拆相、GPU/DRAM 流量。**不看**单纯墙钟。
- **优先"少干活"**（去冗余拷贝、少唤醒、串行省电），而不是"把活干快"（提频、加并行度）——
  后者常把墙钟换成更高的 CPU 总量（见 §5：并行解码要多烧一倍以上 CPU）。
- **硬约束**：`WITH_SIMD=OFF` 是**逐像素一致**的构建前提（部分整数算法在不同实现下舍入不同）
  ⇒ 不要指望"打开 NEON 就快了"；真要 SIMD，必须**逐字节一致**。
  （逐位等价的改写不受此限，例如"每像素一个 32 位掩码字"→"每两像素一个 64 位掩码字"。）
- **现状：解码 worker 数固定为 1**（`kPinnedWorkers`）——旋钮与曲线见 §5，那是"打开旋钮时的知识"。

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
  | `idwt` | **4 成半** | 逆 DWT：每个分量 3 级 lifting（`rfx_dwt_2d_decode_block`，标量 int16，逐行/逐列各一遍，另有一块临时缓冲） |
  | `state` | 约 2 成 | 系数状态拷贝：`sign`（RLGR 原始系数）+ `current`（去量化后），每 tile 约 50KB 的读写 |
  | `color` | 约 1 成半 | `yCbCrToRGB_16s8u_P3AC4R`（整数乘加 + 逐像素 `writePixelBGRX`） |
  | `rlgr` | 约 1 成 | RLGR 位解码（三个分量各 4096 系数） |
  | `upgrade` | 半成 | type 2/3 的逐系数细化 |
  | `dequant+diff` | 百分之几 | 10 段 `lShiftC` + LL3 差分 |

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
- **并行买到的墙钟远少于核数，代价却在 CPU 上**：整屏样本 4 个 worker 只把 tile 解码段压到串行的
  三分之一量级（`dec`），而"解码之外的线程 CPU"是那段墙钟的**十倍量级** ⇒ 池里大部分 CPU 花在争用/
  空转（chunked 投递的共享计数自旋）而不是解码。再加上**多核一起跑时 SoC 会把频率档压低**
  ⇒ "串行慢多少"必须连同 `cpuKHz` 一起读（§7）。
- **串行是省电那一头**：整屏样本慢 2× 出头但 CPU 只有 35%；碎片样本墙钟几乎不变（+4% 量级）而 CPU 只有 45%。
- 已否定：池 fan-out 4→8（`dec`/`本机` 都没有收益）。

## 6. 优化清单与里程碑（按实测占比排）

**顺序的依据**是 `prog2` 的实测占比（§3）：`dec` 之内 `idwt` 约 4 成半、`state`+`color` 共约 3 成半、
`rlgr` 只 1 成 ⇒ **不要从 RLGR 入手**（它最小；GPU 侧那条路另有账，见
[`gpu-accel-plan.md`](gpu-accel-plan.md) §4.4）。

| # | 项 | 占比 | 做法要点 | 正确性门禁 | 出口 |
|---|---|---|---|---|---|
| **C1** | 逆 DWT 向量化 | 4 成半 | even/odd 两段逐元素独立（只有 `(a+b+1)>>1` 需要 int32 中间量）⇒ **逐位等价**改写；顺带把临时缓冲挪进 tile 自己的内存、三级之间少搬一趟 | **① 标量 vs 新实现采样对拍**（`mismatch=0`）+ **② `参考:对比` `bad=0`**（见 §7） | `prog2` 的 `idwt` 绝对量下降、`本机`、帧墙钟 |
| **C2** | `state` + `color` 的逐 tile 缓冲流量 | 共 3 成半 | ① 三次状态写并成一次遍历（或让 RLGR 直写 `sign`、去量化直写 `current`，省掉中间那份）；② `yCbCrToRGB` 的逐像素 `writePixelBGRX` + 整数乘加向量化（同一批系数与移位） | 同上（对拍 + 参考对比） | 同上 |
| **C3** | `update` 的 tile→surface 拷贝 | 1 成半（串行） | keep-dst-alpha 的"每像素一个 32 位掩码字"→**每两像素一个 64 位掩码字**（逐位等价）+ 自动向量化 | `参考:对比` `bad=0`（同样要求逐位等价） | `perFrame decode` |
| — | `upgrade` / `dequant` | 半成 / 百分之几 | 占比小，C1–C3 之后再评估 | — | — |
| — | RLGR | 1 成 | 最小项，最后再看 | — | — |

- 三项都属"**少搬字节 / 同一批算术换组织**"，不是"把活干快"（§0）；向量化只允许**逐位等价**的改写。
- **前置对齐**：C2 里"让 RLGR 直写 `sign`/`current`"会动寄存的布局，和 GPU 阶段化（
  [`gpu-accel-plan.md`](gpu-accel-plan.md) §4.3）是同一块地，动手前先跟那边对齐，别为 CPU 优化把阶段化挡住。
- **之后（不是现在的目标）**：并行那头的重写与"worker 数自适应"（§5 的"前置"）、producer/consumer 流水线；
  它们要建立在 C1–C3 之后的基线上。

**待收尾**：patch 第 14 步（`hmrdpDirtyLeft/Right`：脏区记成"每 tile 行一个 span"而不是逐 tile union）
当初量到**净收益 ~0**；它现在仍在树里，而近几轮 `Vulkan对比` 是 `bad=0`。两条路选一条并重跑两份录像确认：
撤掉（回到逐 tile union 的简单基线），或保留（并把 §4 的那条结论改成"span 形状已定型"）。

## 7. 量测纪律与陷阱

- **正确性门禁 = 参考画面（golden reference）**，两份基线录像各一套：先 `参考:导出` 记下
  `hmrdp_ref_<captureTag>.{hash,bmp}`，之后每轮 `参考:对比` 必须 `bad=0`（逐帧哈希与参考一致）。
  参考文件名带**录制内容指纹**，所以换一份录像必然缺参考、必须重新导出——这就是"同名文件的不同录制
  不能互相背书"的机器保证。
  - ⚠ **它要求"逐位等价"**：参考记录的是 gdi 在被优化之前的输出，所以**只接受逐位等价的改写**
    （这正是 §0 的硬约束）。改了解码语义（哪怕只是舍入不同）就必须**重新导出参考**，并说明为什么可以接受。
  - **`bad=0` 的旧口径（引擎 vs gdi 影子对比）只从代码跑得到**（`GfxReplayRoute::kVulkanCompare`）：
    它仍给出逐像素的 `rgbPx/alphaPx/bbox/maxDelta`，定位"差在哪"时用它；UI 上留给引擎重做之后。
  - 参考模式**不是性能模式**：每帧对整个桌面哈希（`hashMs=` 报出来），这段时间落在 `EndFrame` 里、
    会算进 `compose` ⇒ 读性能数字请用 `参考:关` 那一轮。
- **频率会跟着回放节拍跑**：轻负载样本被压在最低频档、重负载跑满，同一份代码能差数倍。
  ⇒ `mode=fast`（dev 页「跑满」）**不打节拍**；复现 live 的到达节奏用 `realtime`；
  **判读前先看 `cpuKHz=`，不同就不要横向比**。
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
- **dev 页驱动：别用盲点坐标**。先 `dumpLayout` 取控件 `bounds` 再点中心
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）；**按钮在顶栏（y 很小），而状态行会重复同名文字**
  （如 `路线:CPU`），按 `text` 找控件时要取顶栏那个。另外「路线/重新回放/线程/加速」内部都是 `stop + start`，
  **一轮没跑完时点击 = 掐断**。

## 8. 两个被否证的假设（成本该按什么估）

1. **"每次 tile 的 region16 union（O(n²)）是 `update` 的大头"** —— 修正过程：
   先把相邻 tile 索引合并再 union，`unions ≈ tiles`（说明 Progressive 每条消息的 region **基本就是
   每 tile 一个矩形**，没有可合并对象）⇒ 零收益；再改成"每 tile 行一个 span、帧末一次并进 region"，
   `unions` 降了 50 倍以上，而 `update` 只降 ~0.5ms ⇒ **union 总共只值 ~0.5ms**。
   ⇒ 教训：**`update` 的账要按"拷贝字节数"估，不要按"矩形条数"估**。
2. **"delta 折叠（只拷没写过的区域）"** —— 去重 stamp 实测命中 **0 次**（帧内没有重复合成）⇒ 无收益。
