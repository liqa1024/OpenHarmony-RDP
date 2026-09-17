# CPU（gdi）链路的性能与后续工作

本文件是 **CPU 路线**（FreeRDP gdi 解码 + 我们自己的呈现器）的性能口径与**后续工作清单**。
GPU 引擎那条线（RLGR kernel 并行化）在 [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)；
GFX 协议语义以 [`gfx-engine.md`](gfx-engine.md) 为准（尤其 §0.4 / §2.2 / §2.3 / §3），
上屏管线在 [`present-pipeline.md`](present-pipeline.md)。

> 前提：这里的所有数字都是"**同一台真机、同一份录像、同一套探针**"下的相对值；换设备/换样本都要重跑。
> 两份样本（`.cache/hmrdp_gfx_short.bin` 滚动/碎片、`.cache/hmrdp_gfx_video.bin` 整屏视频）
> **每份都要自己过一遍 `Vulkan对比 bad=0`** 才能当基线。

## 1. 每帧工时与账目（判读口径）

CPU 回放的 stats 就是这条线的账：

- `perFrame 本机=… = zgx+parse + decode + compose + present`；`decode` 再拆成
  `prog ms/frame: read / dispatch / wait(block) / update`（FreeRDP 侧导出 `HmrdpProgStat`）。
- `run threads=… libWants=… applies=… resizes=… cpu=…s`：该轮的 decode worker 数与
  **整轮进程 CPU 时间**（能耗代理——只看墙钟会被"并行多烧电但一样快"骗过去）。
- `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp`：**非像素 GFX 命令**
  （`GfxSetupKind`，`hmrdp_gfx_work.*`）。它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 之前先看这行**。
- 单条超过 20ms 的结构命令会自己打一行 `gfx setup: <Name> took … us`（模式切换/整面清零这类卡顿）。

真机基线（`mode=fast`，不含一次性探针）：

| 样本 | `本机`/帧 | 拆相 |
|---|---|---|
| 视频（195 帧，整屏脏，~1325 tile/帧） | 24.5ms | zgx+parse 1.1–1.4 + decode 19.3（read 0.2 / dispatch 0.8 / **wait-block 11.8** / **update 5.6**）+ compose 1.6 + present 2.1 |
| 滚动（150 帧，~49 tile/帧） | 13.5 → **10.5**（§2） | zgx+parse 4.2 → 2.7 + decode 6.4 → 4.8 + compose 0.5 + present 2.5 |

判读顺序：① `setup`（有没有低频重命令）② `kB/frame`/`cmds/frame`（内容是否可比）
③ `wait(block)`（并行解码的真实墙钟）④ `update`（串行合成）⑤ `present`。

## 2. 已做的优化（结果与数字）

都在 `native/scripts/patch-freerdp.ps1` 的对应步骤里，**两份录像都过 `bad=0`**：

| 项 | 位置 | 效果（真机实测） |
|---|---|---|
| tile 任务**分片 + 共享计数器动态领取**（替代"每 tile 一个 WinPR 任务"） | patch 第 10 步 | 视频 `dispatch` 4.0 → 0.78ms/帧 |
| `update_tiles`：不再逐 tile 建 `REGION16`；一次取裁剪表 + 普通矩形求交 + per-tile stamp 去重 | patch 第 10 步 | 视频 `update` 6.3 → 5.6ms/帧 |
| keep-dst-alpha 32bpp 拷贝：每像素一个掩码 32 位字（替代每像素三个字节） | patch 第 10 步（`prim_copy.c`） | 该循环是 `update` 里最热的一段 |
| **`ResetGraphics` 不再重分配 PLANAR scratch**（几何没变就直接返回） | patch 第 12 步（`planar.c`） | 滚动 `本机` 13.5 → **10.5ms**、`feed` 2.13 → **1.63s**、整轮 `cpu` **−17%**、`max` 帧 146 → **115ms** |
| **gdi 主缓冲 = 呈现器自带缓冲**（§6.1 ③ 零拷贝上屏） | app 侧（`hmrdp_presenter.h`、`hmrdp_vk_renderer.*`、`hmrdp_gfx_cpu.*`、`hmrdp_session.*`、`hmrdp_replay.cpp`） | 视频（整屏脏）`present` 2.13 → **0.64ms（−70%）**、`本机` 23.6 → **22.0ms**、`cpu`/轮 31.1 → 30.6s、`fps` 40.0 → 42.9；碎片样本 `present` 2.50 → **1.91ms**、`本机` 12.95 → **12.48ms**、`cpu`/轮 3.83 → 3.69s |
| 归因口径：`prog` 相位 + `setup` 计数 + 每轮 `cpu=` | app 侧（`hmrdp_gfx_work.*`、`hmrdp_replay.cpp`） | 上面四条都是靠它定位的 |

⚠ 已**否决**：`update_tiles` 的"只拷没写过的区域"（delta 折叠）——stamp 去重实测**命中 0 次**
（本样本帧内没有重复合成），折叠无收益。

## 3. 解码线程数：旋钮、能效与实测曲线

- **唯一可用的并行度旋钮**：tile 解码走 WinPR 线程池，**每个 codec 上下文一个池**（`rfx.c` 的
  `CreateThreadpool`）；本平台**不能绑核/绑簇**（没有亲和性接口）⇒ "自适应"只能是**选多少个 worker**。
- **控制面**：patch 第 11 步导出 `HmrdpSetDecodeThreads` / `HmrdpGetDecodeThreads` /
  `HmrdpApplyDecodeThreads(PTP_POOL)` 与 dev 读数；app 侧 `hmrdp_decode_tuning.*` 算自动值；
  设置页「解码线程数」= 0(自动) / 1..8，dev 回放页有「线程」按钮。
  **`n == 1` 走完全串行分支**（不建池、不提交、不唤醒、不等待）。
- **自动值** = `min(性能核数（读 `/sys/.../cpuinfo_max_freq`）或核数, 4)`，上限 4 来自下表。

| worker | 视频 `本机`/帧 | 视频 cpu/轮 | 滚动 `本机`/帧 | 滚动 cpu/轮 |
|---|---|---|---|---|
| 1（串行） | **65.0ms** | **13.7s** | 14.1ms | **2.4s** |
| 2 | 24.7 | 31.9s | 13.3 | 3.8s |
| 3 | 24.1 | 31.6s | 13.8 | 3.9s |
| 4（自动） | 24.4–25.1 | 31.6–31.8s | 13.9–14.0 | 3.9s |
| 6 / 8 | 24.0 / 24.0 | 31.9 / 31.0s | 13.3 | 3.9s |

- **结论**：**≥2 之后墙钟不再变**（原因见 §4，不是"没有核"）；**串行是省电那一头**（视频慢 2.6x
  但 CPU 只有 43%，滚动几乎不变而 CPU 只有 60%）⇒ **当客户端不是瓶颈时（到达侧限速、duty 很低）
  就用 1**；自动值 4 是"两头都不吃亏"的折中。**它不是性能旋钮**，设置页副标题也这么写。
- 顺手否定过：线程池 fan-out 上限 4 → 8（`wait` 11.82 vs 12.04、`本机` 24.47 vs 24.59）没有收益
  ⇒ **保持 4**（能耗口径，patch 第 9 步）。

## 4. 并行效率：为什么"调 worker 数"不是解（**待做**）

> 背景：CPU 链路（FreeRDP gdi）"每个 tile 的固定开销"已经被拿掉（patch 第 10 步：分片派发、
> `update_tiles` 无分配化 + 去重、keep-dst-alpha 拷贝走掩码 32 位字；视频 `本机` 28.96→24.5ms）。
> 剩下的就是**解码本身的并行效率**——调 worker 数不管用，见下。

### 4.1 现象（先解释它，再谈优化）

逐 worker 数的曲线（视频样本）与 cpu 列见 §3 的表；两条结论：

- **≥2 之后墙钟完全不涨**——这**不是"没有更多核"**，而是**并行效率本身到顶**：串行→2 的跳变
  （65.0→24.7ms）已经**超过 2 个 worker 能解释的 2x**，而 2→8 又完全不涨，说明**每个 tile 的有效成本
  随并发度变化**，不是"把同一份工作平均切开"。⇒ **调 worker 数是把问题换个位置暴露，不是解。**
- **并行比串行多烧 ~2.3x CPU**（13.7s → 31.5s/轮）：墙钟赚 2.6x、CPU 亏 2.3x。这个交换在移动设备上
  **只在客户端本身是瓶颈时才划算**；到达侧限速（duty 很低）时应当回落串行（见 §3）。

### 4.1b 第一步量测结果（真机，视频样本，已做完）

- **计数（可信）**：**1325 个 tile/帧**（整轮 ~258k，与 `composited` 1372/帧 一致）⇒ 每个 tile 都是"解码一次 + 合成一次"，
  **没有重复合成**（§6.1 ① 的折叠无收益）。
- **CPU 账（可信）**：串行 14.46s/轮 vs 并行(4) 32.19s/轮（**+17.7s，+122%**），而
  **tile 数完全相同、`update` 并行反而更低**（5.6 vs 9.2ms/帧）、`setup` 可忽略（1.4ms/帧）
  ⇒ 多出来的 CPU **既不在像素搬运、也不在 region 记账、也不在结构命令里**，
  只能在"**并行这条路径本身**"：并发下的解码访存代价，和/或线程池的提交/唤醒/自旋。
- **每 tile 墙钟（由可信用量导出）**：串行时 `(decode 63.5 − read 0.12 − update 9.21)ms ÷ 1325`
  = **40.9µs/tile**；并行(4)时并行段墙钟 12.1ms/帧 ÷ 1325 = **~9.1µs/tile**
  ⇒ **同一份工作在单核串行下比多核并行下贵 2.5–4.5 倍**。这就是"≥2 平顶 + 串行更省 CPU"的机制：
  **瓶颈是访存/缓存干扰，不是核数，也不是池的大小**——与 §4.2(2)(3) 的怀疑方向一致。
- **量测陷阱（写死，不要再踩）**：
  - **`CLOCK_THREAD_CPUTIME_ID` 在本设备不可信**：用它做 per-tile 累计，串行那一轮算出 **21.4s** 的
    tile CPU，而**同一轮进程总 CPU 只有 14.46s**（自相矛盾，差 1.5–2x）。**不要**再拿它做归因。
  - **`clock_gettime` 本身在这台设备上实测 694.9ns/次**（不是 vDSO 级）：per-tile 两次调用 =
    1.4µs/tile ≈ **1.9ms/帧**，直接把被测对象抬起来。⇒ **per-tile 计时这条路放弃**；
    要量单 tile 成本，只能用"单线程场景下的墙钟差"或外部采样（`perf`/`/proc` 采样）。
  - 该探针**已从代码里移除**（`progressive.c` 里只剩 §4.1 的那些 per-message 计时）；
    保留下来的是 **app 侧新增的 `setup` 计数行**（见 §6.5，代价可忽略、结论长期有用）。

### 4.2 待查清单（"先量后改"，每条都先立基线）

1. **先量"每 tile 的 CPU 时间随并发度"**（而不是总时间）：在 tile 回调里用
   `CLOCK_THREAD_CPUTIME_ID` 累计 + 计数，除以该轮 tile 数，得到 **ns/tile(worker=1..8)** 曲线。
   - ns/tile 随并发度上升 ⇒ 争用/带宽（2、3）；不变而总 CPU 翻倍 ⇒ 工作被重复做（4）。
   - **量完就删**：`clock_gettime` 进 per-tile 路径本身就会把被测数字抬起来（早前一轮已实测），
     所以只做一次性实验，不要长期留在代码里。
2. **访存形态**：每个 tile 的缓冲是 `tile->data`(16KB) + `current`/`sign`(各 ~24KB) + 输入位流，
   一帧 ~1.4k 个被访问 tile。多 worker 同时扫这些散布缓冲 → cache line 争用/预取失效，
   **同一份工作在墙钟上变慢**。可试：**per-worker 的连续 scratch**（现在 `progressive->params[]`
   是每 tile 一槽，可再加 per-worker 版本），或让一个 worker **连续吃空间相邻的 tile**
   （现在的共享计数器是"顺次取号"，没有利用空间局部性）。
3. **共享状态**：`progressive_decompress_tile_*` 是否触碰 ctx 级可变字段（如 `progressive->upgrade`
   这类 `RFX_PROGRESSIVE_UPGRADE_STATE`）？有就是隐式串行/假共享；要做的是**按 worker 分片**，
   **不要加锁**（更慢）。
4. **批处理粒度与串并行交替（Amdahl 上限）**：现在**每条 region 一次 fork/join**（632 次/轮），
   而解析与 `update_tiles` 都在 RDP 线程上串行（视频实测 read 0.2 + update 5.6 + 解析 ~1.5
   ≈ 7ms/帧，占 24.4ms 的 ~29%）。正确方向是**让并行段互相重叠**（producer/consumer：worker 解码
   region k 时，RDP 线程解析 k+1 / 合成 k−1），而不是继续加 worker。
   ⚠ 合成的语义约束（同帧重复合成、clip 必须取 REGION 头）见
   [`gfx-engine.md`](gfx-engine.md) §2.2，搬进 worker 前先把顺序语义理清。
5. **硬约束**：`WITH_SIMD=OFF` 是**逐像素一致的构建前提**（`lShiftC_16s_inplace` 等在不同实现下舍入不同，
   见 [`gfx-engine.md`](gfx-engine.md) §2.1/§4）⇒ 不要指望"打开 NEON 就快了"；真要 SIMD，必须证明
   与参考**逐字节一致**，否则 `Vulkan对比 bad=0` 这个门禁的含义就变了。
6. **验收与账**：两份录像 `Vulkan对比 bad=0 rgbPx=0`；性能按 `prog` 桶（read/dispatch/wait(block)/update）
   + 每轮 `cpu=` 报（`cpu=` 就是为这条线加的）。

---

## 5. WinPR 线程池针对鸿蒙的适配（**待做**）

现在的池是"能跑就行"的 Win32 仿真；下面几条直接决定 §4 的天花板。

- **现状（代码级）**：每个 work item = 两次 `calloc`（`TP_WORK` + `TP_CALLBACK_INSTANCE`）+ 入队
  （`Queue_Enqueue` + `CountdownEvent_AddCount`）+ 一次 futex 往返；`WaitForThreadpoolWorkCallbacks`
  等的是**池级全局 CountdownEvent**（**一次 wait 会把整个池抽干**）；worker **只在
  `SetThreadpoolThreadMinimum` 时创建、之后不涨**，**缩小要把全部 worker 撕掉重建**
  （`SetThreadpoolThreadMaximum` → `SetEvent` + `ArrayList_Clear`），没有"空闲渐退"。
- **已做的**：提交粒度从"每 tile"改成"分片 + 共享计数器动态领取"（patch 第 10 步，视频 dispatch
  4.0→0.78ms、`wait` 不退化）——**但池的语义没动**。
- **一次 wait 抽干整池**正是 §4.4 的最大障碍（跨 region 无法重叠）⇒ 至少要有"只等这一批"的接口
  或 per-item 完成计数。
- **鸿蒙侧可用手段（按优先级）**：
  1. **交给系统任务队列**：OHOS 的 `ffrt`（Function Flow Runtime）本身就是"提交任务、由系统决定
     何时/在哪跑"的模型，还能表达依赖与 QoS。若设备可用，**把 tile 任务交给 ffrt 而不是自养 pthread 池**，
     让平台去做大小核/频率决策——这比自己调 worker 数更接近"能效甜点"。按
     [`native-libraries.md`](native-libraries.md) §6 的 Capability 模式做**能力探测 + 优雅降级**。
  2. **QoS 分级**：已有 `HmrdpApplyThreadQoS` 钩子（每个 worker 入口调一次，libhmrdp dlopen `libqos.so`
     注册）。可扩展成**按 duty 动态调整**：到达侧限速时把 worker 降到低 QoS/低优先级，别跟 UI/呈现抢电。
  3. **减少唤醒**：同一 region 内的连续取活已做；再往下是"常驻 worker + 短自旋"，
     但必须量"省下的唤醒 vs 自旋烧的电"，不要凭直觉。
- **不能碰的语义**：**同一个 tile 仍必须由单个 callback 独占解码**（不要把一条 tile 的像素拆给多个
  worker 写），否则 `bad=0` 立刻失去意义。
- **可测指标**：每轮 `cpu=`、`/proc/self/status` 的 `voluntary_ctxt_switches` 差分（futex 唤醒次数）、
  `prog` 的 `dispatch`/`wait(block)` 桶、worker 数（`HmrdpGetDecodeThreadsStats()`）。

---

## 6. 其余候选（冗余搬运 / 折叠 / 流水线 / 内存缓存）

> 目的：把"除了并行度之外还能动什么"一次列清楚，**每条都带依据与门禁**；实施顺序按"收益/风险"。
> 每帧工时基线见 §1（视频 24.5ms = zgx+parse 1.4 + decode 19.3 [read 0.2 / dispatch 0.8 /
> wait-block 11.8 / update 5.6] + compose 1.6 + present 2.1；滚动 13.5→10.5ms）。

### 6.1 A 类：冗余搬运（同一批像素被搬三遍）

一帧的像素在被 GPU 看到之前**经过三次 CPU 拷贝**（视频样本每帧 **20.6MB**，即整屏）：

| # | 拷贝 | 位置 | 本帧代价（视频） |
|---|---|---|---|
| ① | tile → surface | `update_tiles` 的 `freerdp_image_copy_no_overlap` | 约占 `update` 的 1/2（≈2–3ms） |
| ② | surface → primary | `gdi_OutputUpdate` 的 `freerdp_image_scale`（1:1 时退化成行 memcpy） | = `compose` ≈ 1.6ms |
| ③ | ~~primary → presenter staging~~ **已去掉** | `VkRenderer::PresentBgra` 逐矩形 memcpy | 视频 ≈ present 里的 ~1.5ms |

- **③ 已做（零拷贝上屏）**：`gdi` 支持**调用方自带主缓冲**（`gdi_init_ex` / 详见下），于是让**主缓冲就是
  presenter 的 host-visible 缓冲**（桌面尺寸、`stride == 桌面宽 × 4`、常驻映射），gdi 合成的像素**直接落在
  要被 GPU 读的那块内存里**，presenter 不再 memcpy，只录脏矩形的 `vkCmdCopyBufferToImage`
  （`bufferRowLength` 用桌面宽）。实测见 §2：视频 `present` −70%、碎片样本 −24%。
  **实现要点（都是踩过的）**：
  - **接口**：`FramePresenter::AcquireDesktopBuffer(w, h, &stride)` / `BeginDesktopBufferWrite()` /
    `ReleaseDesktopBuffer()`（`hmrdp_presenter.h`，默认实现返回 nullptr ⇒ GLES 后端照旧走 staging）。
    `VkRenderer` 识别"`data` 就是自己那块缓冲"后走直连路径，于是 `PresentBgra` 的签名和调用方都没变。
  - **要**每帧问一次、成功为止**：Vulkan 设备是随 surface 建的，`PostConnect`（gdi_init）时可能还没有
    ⇒ 失败就先让 gdi 自己持缓冲，`EndPaint` 里再挂（`AttachPresenterDesktopBuffer`）。live 与回放共用同一套。
  - **挂接不要用 `gdi_resize_ex`**：它会再调一次 `update_end_paint()`，从帧内调会把 `update->mux`
    的加解锁配对搞乱（递归锁不会死锁，但会永久多出一层占用）。改成**就地换 `primary->bitmap->data`
    / `scanline` / `free` 与 `gdi->stride`/`primary_buffer`**，并把旧缓冲按 gdi 的 free 钩子释放；
    **换之前要把已合成的桌面拷过去**——gdi 的图元会读目标缓冲（memblt / srcalpha / cache 还原），
    换成新缓冲会让后续帧像素变样。
  - **单缓冲要等 GPU 读完**：块缓冲只有一份（原来是每槽一份 staging + 2 帧在飞），所以下一帧写之前必须
    等上一帧的拷贝结束 ⇒ 在 gdi 的 **`BeginPaint`** 里 `BeginDesktopBufferWrite()` 等那个 fence。
    **这个等待实测是免费的**：GFX 路径的 `update_begin_paint` 是在 `gdi_OutputUpdate` 里调的（在
    `EndFrame` 时），即**在哪一帧的解码之后** ⇒ present 到下一次 BeginPaint 之间隔着整帧解码（十几 ms），
    而提交本身只有亚毫秒级。
  - **内存类型**：优先 `HOST_CACHED`（[`gfx-engine.md`](gfx-engine.md) §1），拿不到再 `HOST_COHERENT`
    / 仅 `HOST_VISIBLE`。非连贯类型要 `vkFlushMappedMemoryRanges`；**必须先把脏矩形的字节区间排序合并**
    ——一条矩形的字节跨度是"首行起点→末行末尾"，碎矩形之间重叠得很厉害，逐条刷会变成每帧上百次驱动调用
    （实测合并后视频 `present` 从 843 → 644µs）。
  - **几何变化**（`ResetGraphics` → `gdi_resize`）会退回 gdi 自有缓冲，下一次 `EndPaint` 按新尺寸重新
    挂接（presenter 侧按尺寸重建缓冲）。
  - **前提**：进程内只建一个 `VkDevice`（[`gfx-engine.md`](gfx-engine.md) §1）。设备若被重建
    （换 surface 且既有队列族不再能呈现），gdi 手里的指针会失效——这与引擎侧持有 `VkBuffer`
    的暴露面相同，是既有假设，不是这次新引入的。
  - **live 侧同一套**：`HmrdpPostConnect` 用 `InitGdiWithPresenter`（设备在就直接给缓冲），
    `Session::HandleBeginPaint` / `HandleEndPaint` 负责等待与挂接；回放路由（`GfxCpuDesktop`）同理。
- **② 可以在"表面就是桌面"时跳过**：只有一个表面、`outputMapped` 且 `mappedWidth/Height == 桌面`、
  原点 `(0,0)`、格式与 primary 相同（GFX 全屏会话的常态）时，surface 内容就是桌面镜像，
  **直接呈现 surface**、不写 primary。**风险**：非 GFX 绘制（桌面位图更新等）只写 primary ⇒
  需要一个"本帧是否发生过非 GFX 主缓冲写"的守卫（回放/GFX-only 捕获里天然没有；live 必须加守卫）。
- **① 只在"同帧重复合成"存在时才可折叠，而本样本实测没有重复**（去重 stamp 命中 0 次）⇒
  **不要做 delta 折叠**（这正是先量后改的价值：看起来最像"重复劳动"的一条其实不存在）。

### 6.2 B 类：可折叠 / 可合并（把"矩形数"降下来，下游一起受益）

- `update_tiles` 每帧对 ~1372 个 tile 各做一次 `region16_union_rect(invalidRegion, …)`，而该 region
  在增长 ⇒ **单次 O(n)、一帧累计近似 O(n²)**（1372 次 × 平均 ~1400 个内部矩形）。
  把它改成：**把本消息的 tile 索引按 raster 序（`zIdx` 升序，等价于对索引数组排序）遍历**，
  在"相邻且两者都整块落在 clip 内"时**先合并成一条矩形再 union**：
  - 面积完全不变（只并相邻无缝的 64×64 块，不会多覆盖像素）；被部分裁剪的 tile 不参与合并。
  - 副作用是**整条下游一起变轻**：`gdi_OutputUpdate` 的逐矩形拷贝次数、`hwnd->cinvalid` 的长度
    （present 的 copy region 数）都会从 ~千级降到十级。
  - 门禁仍是两份录像 `bad=0`（脏区的**形状**本来就是自由量，只有面积参与语义）。
- `update_tiles` 里"整 tile 且 clip 全覆盖"已是整块 memcpy；keep-dst-alpha 那条已是掩码 32 位字，
  **交给编译器自动向量化即可**，不要手写 NEON（逐像素一致的约束见 §4.2(5)）。
- **presenter 侧的同类合并**：滚动样本 present 2.5ms 只搬 2.2MB（≈0.9GB/s）——因为每帧 ~250 条
  平均 47px 宽的碎矩形各走一次行拷贝 + 一个 copy region。把同带相邻矩形先并成更长的条
  （字节不增）能同时降"行拷贝调用数"和"copy region 数"；`box` 与逐条的取舍早已量过
  （[`gfx-engine.md`](gfx-engine.md) §2.3），这里要量的是**"合并后的逐条" vs "现在的逐条"**。

### 6.3 C 类：流水线化（Amdahl，收益上限先算再动）

- **解析 vs 解码**：现在每条 region 一次 fork/join（632 次/轮），解析与 `update_tiles` 都在 RDP 线程
  串行（视频 ~7ms/帧 ≈ 29%）。producer/consumer 让 worker 解码 region k 时 RDP 线程解析 k+1 /
  合成 k−1，理论上限就是把这段串行藏起来（≤29%），**但必须先有 §4.2(1) 的 ns/tile 曲线**才知道
  并行段还剩多少可重叠空间。
- **present 已经不等本帧**（fence 是 2 帧前的）⇒ 无需再动；`compose`+`present` 同在 `EndFrame` 内，
  可考虑把 present 的 CPU 部分（录制/提交）与下一帧解析重叠（同上，先量）。

### 6.4 D 类：内存与缓存

- **staging 内存类型**（见 6.1 ③）：改 `HOST_CACHED` 优先是**既定结论的补齐**；
  真机上先用 `ProbeHostMemory` 确认该设备上 `HOST_COHERENT` 与 `HOST_CACHED` 的写/跨行带宽差
  （引擎侧当年量到 20~180 倍）。
- **per-worker scratch**：tile 的 `data`/`current`/`sign` 是每 tile 一槽（16KB + 2×24KB），
  多 worker 同时扫会互相打 cache；改成 **per-worker 连续块** + 让一个 worker 连续吃**空间相邻**的 tile
  （现在的共享计数器是"顺次取号"，没利用空间局部性）。见 §4.2(2)。
- **`gdi_CreateSurface` 的整表面 `memset(0xFF)`**（3120×2080 = 26MB/次）**落在 `setup` 桶的 `create` 里**：
  实测 5–7ms/次（含分配）。⚠ "新建表面后立刻被整块覆盖 ⇒ 不 memset"这类省法**违反协议初值语义**
  （未绘制区域必须是 0xFF），要动必须逐条论证并有 `bad=0` 之外的对拍。
  （同类的"整面 memset/复位"原先藏在 `zgx+parse` 里的部分已经归因并修掉了，见 §6.5。）
- **ZGX**：对已熵编码的图像载荷基本等于拷贝；`prog` 的 `read` 桶已经把它单独分出来了
  （视频 0.2ms —— 说明视频场景这里没问题；滚动样本按 §6.5 归因，主因是 reset）。

### 6.5 `ResetGraphics` 的结构性开销（**已修，保留复盘**）

app 侧新增的 `setup` 统计行（把非像素命令从 `zgx+parse` 里拆出来；实现见
`hmrdp_gfx_work.*` 的 `GfxSetupKind`）第一轮就抓到一条：

- **滚动样本**（150 帧）`setup` 每帧均摊 / 整轮计数：
  `reset=1.49ms(2)`、`cache=0.52ms(16484)`、`blit=0.20(55)`、`create=0.05(2)`、`delete=0.06(3)`、
  `fill≈0(83)`、`map≈0` ⇒ **合计 2.32ms/帧**，而该样本 `zgx+parse` 总共只有 3.5ms/帧。
- ⇒ **两次 `ResetGraphics` 就贡献 1.49ms/帧 = 整轮 223ms ≈ 9% 的算力，单次 ~112ms**；
  它也解释了该样本 `max` 帧 130–170ms 的尖刺（**一次 reset 就是一次 100ms+ 的卡顿**）。
- **视频样本相反**：`setup` 合计 1.4ms/帧（reset 1.2ms/帧，同样只有 2 次/整轮），可忽略 —— 所以这条
  **只对"有 resize/ResetGraphics 的流"重要**（滚动样本正是这种）。
- **已定位（真机实测 + 代码核对）**，两次 reset 分别是 **141.8ms（`surfaces=0`）** 与 **128.6ms（`surfaces=1`）**，
  而同一次 reset 里的 `update->DesktopResize`（`gdi_resize` + 呈现器换尺寸）只有 **17.1ms**：
  1. **不是表面 `memset`**：第一次 reset 时 `surfaces=0`，照样 141.8ms。
  2. **不是 codec 状态复位**：`rfx_context_reset` / `progressive_context_reset` 逐行看都是**只写几个字段**（no-op 级）。
  3. **是 `BITMAP_PLANAR_CONTEXT` 的重分配**：`freerdp_client_codecs_reset`（`core/codecs.c`）会走到
     `freerdp_bitmap_planar_context_reset(context, width, height)`，而它按**当前桌面尺寸**重新
     `winpr_aligned_recalloc` 四块缓冲：`planesBuffer ×4` + `pTempData ×6` + `deltaPlanesBuffer ×4` +
     `rlePlanesBuffer ×4` ⇒ 3120×2080 时 **≈117MB 的"分配 + 清零"**，**而且 `gdi_ResetGraphics` 对
     `context->codecs` 与 `gdi->context->codecs` 各调一次 reset**（离线 CPU 桌面里这两个是**同一个对象**
     ⇒ 同样 117MB 做两遍 ≈ 234MB）⇒ 与实测的 128–142ms **量级吻合**。
     这个 codec 本工程的流**根本不用**（只用 Progressive / ClearCodec / 未压缩）。
- **已做（patch 第 12 步，结论可复用）**：`freerdp_bitmap_planar_context_reset` 在**几何没变且缓冲还在**时
  直接返回——四个缓冲是**每条消息的临时工作区（先写后读）**，既不用重分配也不用重新清零；
  `planes[0] != NULL` 就是"上次分配成功"的凭证（分配失败会退化成原路径）。
  没有改结构体、没有改头文件、没有动编码器契约（编码器仍能在创建时就拿到缓冲）。
  - **实测（滚动样本）**：`本机` **13.5–14.0 → 10.5ms（−22%）**、`zgx+parse` **4.2 → 2.7ms**、
    `reset` **1.5–1.8 → 0.77ms/帧**、`feed` **2.13 → 1.63s（−25%）**、整轮 `cpu` **3.8 → 3.14s（−17%）**、
    `fps` 42–44 → **47.8**、`max` 帧 **146–164 → 115ms**；慢事件日志从 **2 条（141.8+128.6ms）
    降到 1 条（143.2ms）**。
  - **视频样本**：`本机` 24.5ms（在噪声内；它的 `reset` 只有 1.2 → 0.74ms/帧，本来就只有 2 次）。
  - **两份录像都过 `bad=0 rgbPx=0`**（滚动 checks=5 / 视频 checks=6）。
  - **剩下那一次**（143ms）是**真实几何变化**（回放页的 surface 1982 高 vs 录像里的桌面 2080，
    是 dev 路线的产物）；live 里 `ResetGraphics` 通常就是客户端请求的尺寸 ⇒ 这条不会有成本。
    若哪天要连它一起省，才需要"首次使用才分配"（要注意编码器路径依赖创建时分配）。
- ⚠ 不要用"关掉 planar codec"来省（`gdi_SurfaceCommand_Planar` 会拿到空上下文；协议面也别乱动协商）。
- ⚠ 表面对比那一半（`memset(surface->data, 0xFF)`）**不能省**：未绘制区域必须是 0xFF（协议初值语义）。
- **一般化教训**：`ResetGraphics`/模式切换这类**低频但重**的路径，要按"每次调用分配多少字节"去查
  （这次是 117MB × 2 个 codec 集合），别只看它出现几次；而且**"两个 codec 集合"是不同对象**
  （rdp context 的与 GFX context 的），所以同一个 reset 会把同样的重活干两遍。

---

## 7. 实施顺序（按 2026-09 第一步量测修正）

1. ~~planar scratch 不再随 `ResetGraphics` 重分配~~ **已完成（patch 第 12 步）**：滚动样本
   `本机` 13.5–14.0 → **10.5ms（−22%）**、`feed` 2.13 → **1.63s**、整轮 `cpu` 3.8 → **3.14s**、
   `max` 帧 146–164 → **115ms**；两份录像 `bad=0`。细节见 §6.5。
2. ~~**A-③：主缓冲 = presenter 缓冲**（§6.1）~~ **已完成**：去掉 primary→staging 那一遍拷贝，
   顺带把类型改成 `HOST_CACHED` 优先 + 脏区间合并刷。视频（整屏脏）`present` 2.13 → **0.64ms**、
   `本机` 23.6 → **22.0ms**；碎片样本 `present` 2.50 → **1.91ms**、`本机` 12.95 → **12.48ms**；
   两份录像 `bad=0 rgbPx=0`。细节与约束见 §6.1 ③。
3. **B：区域矩形合并**（§6.2）——`update_tiles` 里按 raster 序合并相邻整块 tile 再 union，
   同时降低 `update`、`gdi_OutputUpdate` 的逐矩形拷贝、`cinvalid` 长度与 present 的 copy region 数。
4. **C：producer/consumer**（§6.3）——除重叠之外，它还能把"解码的工作集"与"解析/合成的工作集"分开，
   直接缓解 §4.1b 量出来的**访存/缓存干扰**（这是并行效率的真正瓶颈）。
5. **A-②：带"非 GFX 主缓冲写"守卫地跳过 primary**（§6.1）——收益与 ③ 重叠，风险更高，放最后。

## 8. 量测纪律与陷阱（血泪版）

- **`bad=0` 是一切的入口**：两份录像 + `Vulkan对比`。任何性能数字只有在那一轮 `bad=0` 的前提下才算数。
- **`clock_gettime` 在这台设备上实测 ~694.9ns/次**（不是 vDSO 级）⇒ **per-tile 计时这条路放弃**：
  两次调用就有 1.4µs/tile ≈ 1.9ms/帧，会把被测对象本身抬起来。要量单 tile 成本，只能用
  "单线程场景下的墙钟差"或外部采样（`perf` / `/proc` 采样）。
- **`CLOCK_THREAD_CPUTIME_ID` 在本设备不可信**：串行那一轮用它累计出 **21.4s** 的 tile CPU，而同一轮
  进程总 CPU 只有 **14.46s**（自相矛盾，差 1.5–2x）。**不要**再拿它做归因。
- **探针只做一次性实验、量完就删**（per-tile 探针已经这么删过一次）；长期保留的只有 per-message
  计时（每条消息 4~5 次）和 app 侧 `setup` 计数（每条命令一次，可忽略）。
- **`present` 之后紧接的节拍睡眠在 `gdi_EndFrame` 里**：不扣掉就会把节拍算进 `compose`（滚动样本曾虚高
  8ms/帧，真值 ~0.5ms）。`GfxWorkMeter::OnPace` 就是为此存在；判读前先看 `pump … ms (paced … ms)`
  里 paced 是否为 0。
- **不要用"跳过某条 dispatch/步骤 + 差值反推"做归因**（依赖关系会变），要用计数器 + 相位桶。
- **每份新捕获先自己过 `bad=0`** 才能当基线（同名文件的不同录制不能互相背书）。
- **A/B 必须同会话内做，不要拿历史日志当基线**：文件名会复用，  同名 `.cache/hmrdp_gfx*.bin` 的帧数、
  `cmds/frame`、脏区形态可以完全不同（同一段代码在两份录制上 `本机` 就能差 1ms 以上），而且设备的后台
  负载/温度也会漂。
  做法是**把待测特性关掉再各跑 2~3 轮**（本次是让 `VkRenderer::AcquireDesktopBuffer` 直接返回 nullptr，
  即退回 staging 路径），取中位数比。判读只看**同一轮 `(running=0)`**。
- 回放页的「路线 / 重新回放 / 线程」按钮内部都是 `stopReplayTest()` + `start`；**一轮没跑完时点击 = 掐断**，
  性能数字只取 `(running=0)` 的整轮。
