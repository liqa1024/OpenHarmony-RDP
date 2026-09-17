# CPU（gdi）链路：成本结构、量测口径与遗留问题

**CPU 路线** = FreeRDP gdi 解码 + 我们自己的呈现器。本文件是这条线的**知识**（钱花在哪、怎么量、
哪些已经定型、还剩什么）。协议语义看 [`gfx-engine.md`](gfx-engine.md)、上屏看
[`present-pipeline.md`](present-pipeline.md)、GPU 引擎侧看
[`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)、本机/拆相口径也见
[`session-and-input.md`](session-and-input.md)。

## 0. 优化判据（决定取舍，不是 fps）

**把每帧的计算成本降下来 = 能耗降下来**；fps 只要"够用"（跟得上服务端的到达节奏、不卡）。

- **判收益看**：每轮 `cpu=`（进程 CPU 时间，能耗代理）、`本机` 拆相、GPU/DRAM 流量。**不看**单纯墙钟。
- **优先"少干活"**（去冗余拷贝、少唤醒、串行省电），而不是"把活干快"（提频、加并行度）——
  后者常把墙钟换成更高的 CPU 总量（§5 实测：并行解码多烧 2.3x CPU）。
- **旋钮应自适应而非取中值**：如解码 worker 数应跟"到达侧是否限速"走。
- **硬约束**：`WITH_SIMD=OFF` 是**逐像素一致**的构建前提（`lShiftC_16s_inplace` 等在不同实现下舍入不同）
  ⇒ 不要指望"打开 NEON 就快了"；真要 SIMD，必须**逐字节一致**，否则 `bad=0` 门禁的含义就变了。
  （逐位等价的改写不受此限：如"每像素一个 32 位掩码字"→"每两像素一个 64 位掩码字"。）

## 1. 账目与口径

CPU 回放的 stats 就是这条线的账：

| 行 | 含义 |
|---|---|
| `perFrame 本机=… = zgx+parse + decode + compose + present  (+ sync … blocked)` | **本机 = 处理时间**；各拆相相加**等于**本机，`sync` **不计入**（见下） |
| `prog ms/frame: read / dispatch / wait(block) / update  (calls= unions= tiles=)` | `decode` 的内部：`read` 输入位流、`dispatch` 投递 tile、`wait(block)` 等 worker（= 并行段真实墙钟）、`update` = `update_tiles` 整段（× `tiles`/`unions` 计数） |
| `run … threads=… cpu=…s  cpuKHz=…` | 该轮的 worker 数与**整轮进程 CPU 时间**；`cpuKHz` 是本轮拿到的 SoC 频率档（见 §7） |
| `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp` | **非像素 GFX 命令**（`GfxSetupKind`）；它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 前先看这行** |
| `gfx setup: <Name> took … us` | 单条 >20ms 的结构命令（模式切换/整面清零这类卡顿） |
| `uploaded/box/rectlist/truncated`、`present=` | 上屏侧：实际交给呈现器的字节、框字节、是否用逐条矩形、上屏耗时 |

- **`sync` = 等 GPU 放开主缓冲**（`BeginPaint` 里的 `BeginDesktopBufferWrite`；CPU 路线让 gdi 直接合成进
  呈现器缓冲，所以下一帧写之前要等上一帧读完）。它是**阻塞**不是处理 ⇒ 单独一项；
  **帧的整段墙钟 = `本机` + `sync`**（回放再加节拍睡眠）。
- **`pace` 是唯一直接剔除的项**：回放的人为节流不是客户端工作。
- **判读顺序**：① `setup` ② `kB/frame`/`cmds/frame`（内容是否可比）③ `wait(block)` ④ `update`
  ⑤ `present` ⑥ `sync`。

## 2. 当前基线（标定用，不是结论）

`.cache/hmrdp_gfx_video.bin`（195 帧，整屏脏，~1330 tile/帧）与 `.cache/hmrdp_gfx_short.bin`
（150 帧，碎片）；`mode=fast`（**跑满、不打节拍**），`cpuKHz≈418000-1200000`：

| 样本 | `本机`/帧 | 拆相 |
|---|---|---|
| 视频 | **20.8ms**（`fps` 45） | zgx+parse 0.8 + **decode 19.2** + compose 0.12 + present 0.66（+ `sync` 31µs） |
| 碎片 | **9.4ms**（`fps` 76） | 2.5 + 5.3 + 0.09 + 1.5（+ **`sync` 2.7ms**） |

⇒ 视频样本里 **decode 占 92%**：任何"上屏/合成"侧的优化都不可能显著改变这条线。

## 3. 一帧的钱花在哪

- **decode（视频 19.2ms/帧）**：拆成 `read 0.15 + dispatch 0.7 + wait(block) 11.5 + update 6.0`。
  - `wait(block)` 是并行解码的**墙钟**（RDP 线程等 4 个 worker）。
  - **`update` 的大头是像素拷贝，不是记账**：`update` 6.4ms 里 ≈5.5ms 是
    `freerdp_image_copy_no_overlap` 把每个 tile（64×64×4B）拷进 surface，合计 **~5.6MB/帧**；
    因为带 `KEEP_DST_ALPHA` 掩码（每像素一次掩码写）而不是 memcpy，实测只有 **~1GB/s**。
    region16 记账只占 **~0.5ms**（见 §8 的两次修正）。
- **compose（0.1ms）**：`gdi_OutputUpdate` 的 surface→primary 合成；全屏会话里它已经被
  “桌面镜像 surface 直接合成进 primary”整段消掉（§4）。
- **present（CPU 0.6–1.4ms）**：~90% 是**固定的 Vulkan 调用**（acquire 275 + present 172 + 录制 75 +
  submit 68 ≈ 0.6ms），不是我们能删的活；`flush` 只有 1.5–3µs。**GPU 侧另算**：每帧 `copy`（20.6MB
  buffer→image）1.75–2.4ms + `blit`（clear 整张 + letterbox quad）0.8–1.1ms，**blit 与脏区无关**。
  ⇒ CPU 侧已经没有可压的；GPU 侧被 20ms 的 CPU 工作藏住（`present` 的 GPU 时间 ≠ `本机` 里的 0.6ms）。
- **`setup`（非像素命令）**：低频但可能很重。`ResetGraphics` 这类路径要按**"每次调用分配多少字节"**查：
  曾经的 112–142ms 尖刺是 `BITMAP_PLANAR_CONTEXT` 按桌面尺寸重分配 ≈117MB，
  而 `gdi_ResetGraphics` 会对**两个 codec 集合**（rdp context 的与 GFX context 的，离线里是同一个对象）
  各做一遍 ⇒ 234MB。现在几何没变就直接返回（`planes[0] != NULL` 即"上次分配成功"）。
  ⚠ 表面对比那一半（`CreateSurface` 的 `memset(surface->data, 0xFF)`）**不能省**：未绘制区域必须是 0xFF。
  ⚠ 不要靠"关掉 planar codec"来省（`gdi_SurfaceCommand_Planar` 会拿到空上下文）。

## 4. 已定型的几件事（不要再动，连同约束）

- **脏区形状：零拷贝路径恒发合并 box；staging 路径保持逐条矩形**。原因：有 memcpy 时成本在**字节**
  （box 会多搬 3.6 倍），零拷贝后成本在**条数**（每条 = 一个 copy region + 一次 flush），而 box 有上界
  （最坏 = 一次整屏读）。由 `FramePresenter::usesDesktopBuffer()` 分流。
  ⇒ "把碎矩形并成长条"这条**作废**。
- **零拷贝上屏：gdi 主缓冲 = 呈现器的 host-visible 缓冲**（`gdi_init_ex` + 呈现器自带缓冲）。
  实现约束（都要遵守）：
  - 呈现器**每帧问一次、成功为止**（Vulkan 设备随 surface 建，`gdi_init` 时可能还没有），
    live 与回放共用 `InitGdiWithPresenter` / `AttachPresenterDesktopBuffer`。
  - **挂接不要用 `gdi_resize_ex`**（它会再调 `update_end_paint`，把 `update->mux` 的配对搞乱）；
    就地换 `primary->bitmap` 的 data/scanline/free 与 `gdi->stride`，**换前把已合成的桌面拷过去**
    （gdi 图元会读目标缓冲）。
  - **单缓冲 ⇒ 下一帧写之前要等上一帧的 GPU 读完**（§1 的 `sync`），放在 gdi `BeginPaint`；
    而 GFX 的 `begin_paint` 在 `EndFrame` 里（即整帧解码之后）⇒ 解码足够长时这笔等待免费。
    ⚠ 轻样本跑满时会顶到显示/GPU 上限，这笔等待变成 2–3ms（`sync` 单列，别当成 compose）。
  - 内存类型 `HOST_CACHED` 优先；非连贯时要 flush，**按脏区合并成一个区间刷**（逐条刷会变成每帧上百次
    驱动调用；cache flush 只写回脏行，多出来的干净行免费）。
  - 几何变化（`ResetGraphics` → `gdi_resize`）会退回 gdi 自有缓冲，下一次 `EndPaint` 按新尺寸重挂。
  - 前提：**进程内只建一个 `VkDevice`**（见 [`gfx-engine.md`](gfx-engine.md) §1）。
- **桌面镜像 surface 直接合成进 primary**（全屏 GFX 会话的常态：只有一个 surface、`(0,0)` 1:1、
  格式/行距与桌面相同 ⇒ 它**就是桌面**，那趟逐矩形 `freerdp_image_scale` 纯属白搬）。做法是把这个
  surface 的 `data` 指向 `gdi->primary_buffer`，解码器写的就是呈现器要上传的内存。**生命周期是重点**：
  - 凭证就是 `surface->data == gdi->primary_buffer`；任何一条不满足就退回逐矩形拷贝（最坏=改之前）。
  - `gdi_ResetGraphics` **保留** surface 并 `memset(surface->data, …)`，而它调的 `DesktopResize`
    会**换掉 primary** ⇒ 必须**换之前**记下谁在共享、换之后重指向或让它自己分配（否则是"向已释放内存
    memset"，而且是写进呈现器的 Vulkan 缓冲）。
  - `gdi_DeleteSurface` 不能释放共享缓冲；出现**第二个** surface 时先解除共享（否则合成第二个会覆盖它）。
  - 全在 `libfreerdp/gdi/gfx.c` 内部，**不动头文件也不动 app**，因此对任何呈现器都成立。
- **`update_tiles` 的 region16 记账不是瓶颈**（只有 ~0.5ms/帧）：不要再去合并矩形/换 region 结构
  （两次实测都证明收益在噪声里，见 §8）。

## 5. 并行解码：能效曲线与结论

唯一可用的并行度旋钮是 **tile 解码的 worker 数**（WinPR 线程池，每个 codec 一个池；本平台**不能绑核/绑簇**）。
控制面：patch 导出的 `HmrdpSetDecodeThreads/Get/Apply`，app 侧 `hmrdp_decode_tuning.*`，设置页「解码线程数」
（0=自动 / 1..8），dev 回放页有「线程」；**`n == 1` 走完全串行分支**（不建池、不提交、不唤醒、不等待）。
自动值 = `min(性能核数或核数, 4)`。

| worker | 视频 `本机`/帧 | 视频 `cpu`/轮 | 碎片 `本机`/帧 | 碎片 `cpu`/轮 |
|---|---|---|---|---|
| 1（串行） | 65.0ms | **13.7s** | 14.1ms | **2.4s** |
| 2 | 24.7 | 31.9s | 13.3 | 3.8s |
| 4（自动） | 24.4–25.1 | 31.6–31.8s | 13.9–14.0 | 3.9s |
| 6 / 8 | 24.0 | 31.9 / 31.0s | 13.3 | 3.9s |

- **≥2 之后墙钟完全不涨**：串行→2 的跳变已超过 2x，2→8 又不涨 ⇒ **每个 tile 的有效成本随并发度变化**，
  不是"把工作平均切开"。机制是**并发下的访存/缓存干扰**（同一份工作单核串行比 4 核并行慢 2.5–4.5 倍）
  ⇒ **调 worker 数是把问题换个位置暴露，不是解**。
- **串行是省电那一头**：视频慢 2.6x 但 CPU 只有 43%；碎片墙钟几乎不变而 CPU 只有 60%。
  ⇒ **到达侧限速（duty 低）时应回落 1**；自动值 4 是"两头都不吃亏"的折中，**它不是性能旋钮**。
- 已否定：池 fan-out 4→8（`wait`/`本机` 都没有收益）⇒ 保持 4。

## 6. 开放问题（按能耗收益排）

1. **`update` 里的 tile→surface 拷贝**：~5.5ms/帧且**串行在 RDP 线程**（整帧 ~26%）。
   做法：把 keep-dst-alpha 的"每像素一个 32 位掩码字"改成**每两像素一个 64 位掩码字**
   （`dst = (dst & 0xFF000000FF000000) | (src & 0x00FFFFFF00FFFFFF)`，逐位等价）+ 让它自动向量化；
   预期 5.5 → ~2.5ms（整帧 −14%）。门禁 `bad=0`。
2. **解码 worker 数按"到达侧是否限速"自适应**（§5）：到达受限时用 1 ⇒ CPU −40…57%（墙钟在预算内）。
   判据用已有的 duty/到达间隔，加滞回；旋钮与热切换早就有。
3. **producer/consumer（流水线）**：现在**每条 region 一次 fork/join**，解析与 `update_tiles` 都在 RDP 线程
   串行（视频 ~7ms/帧 ≈ 30%）。让 worker 解码 region k 时 RDP 线程解析 k+1 / 合成 k−1，理论上限是把这段
   串行藏起来；它还能把解码工作集与解析/合成工作集分开，缓解 §5 的访存干扰。
   ⚠ 合成的顺序语义（同帧重复合成、clip 必须取 REGION 头）见 [`gfx-engine.md`](gfx-engine.md) §2.2。
4. **WinPR 线程池 / 平台任务队列**：现在的池"能跑就行"（每个 work item 两次 calloc + 一次 futex 往返；
   `WaitForThreadpoolWorkCallbacks` 等的是**池级全局计数**，**一次 wait 抽干整池** ⇒ 跨 region 无法重叠；
   worker 只在设 minimum 时创建、缩小要全撕重建）。OHOS 侧更对路的是 **`ffrt`**（提交任务、由系统决定
   何时/在哪跑），按 [`native-libraries.md`](native-libraries.md) §6 的 Capability 模式探测 + 降级；
   其次是 **QoS 分级**（`HmrdpApplyThreadQoS` 已有，可按 duty 动态调）。
   ⚠ 不能碰：**同一个 tile 仍必须由单个 callback 独占解码**，否则 `bad=0` 失去意义。
5. **不划算（已量，别再投入）**：present 整幅重上屏（GPU 侧 ~0.9ms，被 CPU 藏住；且 `bad=0` 覆盖不到
   呈现器）见 [`present-pipeline.md`](present-pipeline.md) §4.3；`update_tiles` 的 union/矩形合并（§4）；
   delta 折叠（去重 stamp 实测命中 0 次）。

**待收尾**：patch 第 14 步（把 per-tile 的 `region16_union_rect` 换成"每 tile 行一个 span"）已实现并量过，
**净收益 ~0**（`update` −0.5ms、`uploaded` +4%）且**未过 `bad=0`** ⇒ **撤掉它**，把基线恢复到已过门禁的状态。

## 7. 量测纪律与陷阱（血泪版）

- **`bad=0` 是一切的入口**：两份录像 + `Vulkan对比`。性能数字只有在该轮 `bad=0` 的前提下才算数；
  **每份新捕获先自己过 `bad=0`**（同名文件的不同录制不能互相背书）。
- **频率会跟着回放节拍跑**：给每帧固定预算、完得早就补睡 ⇒ 轻负载样本被压在最低频档、重负载跑满，
  同一份代码能差 **3x**（`vkAcquireNextImageKHR` 255→560µs 之类）。⇒ `mode=fast` **不打节拍**（跑满），
  复现 live 的到达节奏用 `realtime`；**判读前先看 `cpuKHz=`，不同就不要横向比**。
  - **只有同一次会话里的 A/B 才有效**：把待测特性关掉各跑 2–3 轮取中位数；只看 `(running=0)` 的整轮。
  - 跨样本比 `present` 的"碎片比整屏慢"就是被频率污染的例子，不是结论。
  - 用 QoS 去修频率**无效**（实测毫无变化）。
  - 跑满后轻样本会顶到显示/GPU 上限（碎片 ~72fps > 面板 60Hz）⇒ **A/B 纯 CPU 侧的改动要用 CPU 受限的
    样本（视频）**，否则收益被 GPU/合成器吃掉。
- **计时器本身会被测出来**：本设备 `clock_gettime` ≈ **694.9ns/次**（非 vDSO 级）⇒ **per-tile 计时这条路
  放弃**（两次调用 = 1.4µs/tile ≈ 1.9ms/帧）。要量单 tile 成本只能用"单线程墙钟差"或外部采样。
  `CLOCK_THREAD_CPUTIME_ID` 本设备**不可信**（串行那轮累计出 21.4s，而整轮进程 CPU 只有 14.46s）。
- **探针只做一次性实验、量完就删**；长期保留的只有 per-message 计时、app 侧 `setup` 计数、
  `unions/tiles` 计数和 present 的 GPU 时间戳（都便宜且结论长期有用）。
- **不要用"跳过某条 dispatch/步骤 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
- **回放节拍若落在 `gdi_EndFrame` 里，必须扣掉**（否则算进 `compose`）：`GfxWorkMeter::OnPace` 为此存在；
  判读前先看 `pump … ms (paced … ms)` 里 paced 是否为 0。`sync` 同理（见 §1），**别把"等 GPU"当"合成贵"**。
- **墙钟反推的"GPU 时间"不算 GPU 时间**：present 曾由 `sync` 反推出"GPU ≈10ms/帧"，上时间戳后真值是
  **2.6–3.5ms**，其余是排队。
- **dev 页驱动：别用盲点坐标**。会话窗口可缩放（安装后常是 1404×936 而不是全屏），按全屏算的坐标会落到
  桌面/别的窗口上——症状是"点了没反应"甚至把应用窗口关掉，**看起来像崩溃**。先 `dumpLayout` 按 `text`
  找控件 `bounds` 再点中心；另外「路线/重新回放/线程」内部都是 `stop + start`，**一轮没跑完时点击 = 掐断**。

## 8. 两次被实测推翻的"想当然"（留着别再踩）

1. **"每帧 1372 次 region16 union ⇒ O(n²) 是 `update` 的大头"** —— 两次修正：
   - 先试"把 tile 索引按 raster 序合并相邻 tile 再 union"：`unions ≈ tiles`（≈1.07 次/tile），
     说明 Progressive 每条消息的 region **基本就是每 tile 一个矩形**，没有可合并的对象 ⇒ 零收益，撤回。
   - 再试"每 tile 行一个 span、帧末一次并进 region"：`unions` **286014 → 5490（−52x）**，而 `update`
     只从 6.44 → 5.97ms ⇒ **union 总共只值 ~0.5ms**；`update` 的大头是 §3 的逐像素拷贝。
   ⇒ 教训：**`update` 的账要按"拷贝字节数（tiles × 4KB/帧）"估，不要按"矩形条数"估**。
2. **"delta 折叠（只拷没写过的区域）"** —— 去重 stamp 实测命中 **0 次**（帧内没有重复合成）⇒ 无收益。
