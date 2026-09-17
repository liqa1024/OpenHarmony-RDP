# 上屏管线（present）与后续工作

本文件是**上屏这一段**（从「本帧要显示哪些像素」到「swapchain / `vkQueuePresentKHR`」）的口径与交接。
协议/解码语义见 [`gfx-engine.md`](gfx-engine.md)（§0.4、§2.3、§6、§7），构建与验证见
[`build-and-verify.md`](build-and-verify.md)。

> 前提：**GPU 引擎与 GPU 回放只在真机验证**；模拟器上的 Vulkan 会按标准接口谎报能力，不在支持范围。

## 1. 目标：两条路线共用一套上屏实现，且 GPU 侧不能比 CPU 侧慢

CPU 路线（gdi）与 GPU 路线（引擎）**共用同一个 `VkRenderer`**，且**只有一条上屏实现**：

- **picture 图**： FreeRDP 的 BGRA 字节序，格式恒为 `B8G8R8A8_UNORM`（`kPictureFormat`）。生产者可以是
  CPU 帧（脏区 memcpy 进 staging 再拷）或引擎（表面 → picture 的**设备内**拷贝）；**通道序由图像格式
  承担**，shader 不做 R/B 交换（见 [`gfx-engine.md`](gfx-engine.md) §2.3）。
- **上屏**：采样 picture 的 letterbox quad（viewport 做 letterbox），写入 swapchain 图像，再
  `vkQueuePresentKHR`。**不要**改用 `OH_NativeWindow_*` / `FlushBuffer(..., Region)`（那是 CPU/native
  生产者路径，生产者照样要填满 buffer，`Region` 只是给合成器的提示）。
- **帧槽**：引擎 `kSlots = 2`：每槽各有 command buffer / fence / frame semaphore / engine-chain
  semaphore / blit-done semaphore，以及 staging arena、descriptor pool、时间戳池、延迟回收列表、
  "compute 在飞"标志**各一份**（`FlushAll()` 供 CPU 读回路径等所有在飞槽）。
- **picture ping-pong**：每个槽一张 picture；每张带"另一张拿到而自己错过的矩形"账
  （`pictureMissing[图][surface]`），compose 时写 `本帧脏区 ∪ 该图错过的脏区`，随后清空自己、把本帧
  脏区交给另一张。**账上只存矩形、像素一律从当前表面拷**（表面已是最新）。
- **设备侧握手（CPU 不等待）**：
  - `engineChain[slot]`：每次提交都 signal、**下一次提交** wait —— surfaces 与解码 scratch 跨帧读改写，
    帧间顺序由它保证（不要用"等上一次 blit"来代替，那会把 compose 与 blit 也串起来）；
  - `frameSemaphores[slot]`：帧提交 signal、该帧的 blit wait；
  - `blitDone[slot]`：引擎交给 presenter 的令牌，presenter 在 blit 结束时 signal，**两帧后**这张 picture
    被重写前才 wait（picture 复用的保护）。present 失败必须 `AbandonBlitDoneHandoff()`，否则令牌永不被
    signal。
  - CPU 读回路径（`ReadScreen`、ClearCodec/cache 的 RMW、`Reset`/`ResetGraphics`）走 `FlushAll()`
    等所有在飞槽。
  - **"CPU 不等待"只对设备侧的件成立：主机写缓冲的那两条线必须等**。"上一帧仍在飞的读"和"本帧的
    写"是同一块内存时（CPU 路线的零拷贝桌面缓冲、引擎的 mapped 表面），等待点必须落在**本帧第一次
    写之前**，且**每个提交窗口只等一次**：
    - CPU 路线：`GfxWorkSetFrameBeginHook`（挂在 live 与回放共用的包装层，见 [`cpu-path.md`](cpu-path.md) §4）；
    - 引擎：`Impl::SyncForCpuAccess()`（提交臂 `submissionSinceHostDrain`，该提交后的第一次主机访问
      `FlushAll()`）——合成是 **transfer** 不是 dispatch，所以"compute 在飞"那套判断盖不住它。
    ⚠ 这类顺序**不在 `bad=0` 的覆盖范围内**（对比读的是 gdi 自己的缓冲与引擎 picture，两者都还在），
    判断只能靠机制 + 计数器；违反时的形状见 [`cpu-path.md`](cpu-path.md) §4。

## 2. 两条路线的上屏**构成**（不是哪个更快）

两边的 `present` 由不同的件组成，比较时必须按件看，不能比总数（`mode`／样本／频率一变总数就不可比）：

| 件 | CPU 路线（gdi + `PresentBgra`） | GPU 路线（引擎 + `PresentImage`） |
|---|---|---|
| 脏区像素进 GPU | 无 memcpy：gdi 直接合成进 presenter 的 host-visible 缓冲（零拷贝上屏，见 [`cpu-path.md`](cpu-path.md) §4） | 引擎在**设备内**把表面合成进 picture，无 CPU 搬运 |
| 脏区记账/录制 | 只发一个合并 box（零拷贝后按**条数**算） | `presentSplit compose`（每帧扫脏区 + ping-pong damage 账） |
| 提交 | 含在 `present=` 里（绝大部分是固定的 Vulkan 调用） | `presentSplit flush` |
| letterbox quad + present | 同一份代码 | 同一份代码（`presentSplit blit`，含 acquire/描述符/录制/提交/present） |
| 是否等本帧 GPU | 不等（fence 落后 2 帧） | 不等（靠 `engineChain` 保证帧间顺序） |

- **CPU 侧的 `present=` 已没有可压的余地**：拆到 Vulkan 调用一级几乎全是 acquire/record/submit/present
  的固定开销，`flush` 只有 µs 级（数字见 [`cpu-path.md`](cpu-path.md) §3）。
- **GPU 侧的 `blit` 是"整幅重上屏"**（clear 整张 swapchain + 采样整幅 picture），**与脏区无关**；
  结论是**不值得优化**（§4.3）。
- **曾经的主要病灶是把整帧等待放在 present 里**（每帧在 present 中 `Flush()` = submit **+** wait），
  改为分离的 `engineChain` 握手后，`flush` 桶从十毫秒级降到百微秒级。两条路线整帧仍差的部分
  **与上屏无关**：GPU 路线还多跑一遍解码 kernel。

## 3. 引擎侧脏区记账的成本来源

**CPU 侧为什么不需要这笔计算**：gdi 的脏区是**增量维护**的。每条命令写入时
`gdi_InvalidateRegion(hdc, x, y, w, h)` 把矩形并进该表面**持久的 `invalidRegion`（region16）**；
帧末 `gdi_StartFrame/EndFrame` 才 `gdi_UpdateSurfaces` → `gdi_OutputUpdate`：
`region16_intersect_rect(invalidRegion, surfaceRect)`（裁到 mapped 尺寸）→ `region16_rects()` 取
**已合并、已裁剪**的矩形表 → 按矩形 memcpy 进 staging + 录 N 个 `VkBufferImageCopy`。
**合并发生在写入那一刻、用的还是它本来就需要的结构**，每帧只剩一次"取列表 + 顺序遍历"。

**引擎侧现在是怎么写的**（`GfxVkDesktop::Impl`）：脏区是

1. `std::vector<Surface::DirtyRect> dirtyRects`（**原始 mark**，每次写入 push 一条，有上限；
   Progressive 一条消息按**每个解码 tile** 各推一条 ⇒ 整屏 I 帧可达数千条）；
2. 外加一份**并行的 union box**（`meta.dirtyLeft/Top/Right/Bottom`），只为"矩形太长时退回盒子"服务。

每帧 `Compose()` 对每个表面做（同一份信息被扫多遍 + 一次排序）：

1. **`CoalesceRects()`**：对全部原始 mark **排序**再两趟合并；**box 模式下这个结果只用来统计与兜底
   判断，并不需要列表本身**；
2. 读 `meta.dirty*` 作为兑现盒；
3. 逐矩形 `MakeScreenCopyRegion()`：clamp + 计算 dst + `MarkScreenDirty()`；
4. **ping-pong 的 damage 账**：`pictureMissing[pic][surface]` 的 append/clear + 缺失矩形并集
   （还要在 box 模式下把缺失矩形的 hull 并进盒子）。

⇒ 这笔成本是**记账与录制**，不是带宽（`flush` 只有百微秒级；ping-pong 让字节翻倍但字节便宜）。

## 4. 待办（按收益/风险排序）

### 4.1 用"增量 region16"重写引擎的脏区账（目标：上屏 ≤ CPU）

**思路**：把 §3 的 1/2/4 换成 gdi 那套——**在写入时增量合并，而不是每帧重算**：

- `Surface` 持有一个 **`REGION16 invalidRegion`**（FreeRDP 的 region API；引擎已 include
  `freerdp/codec/region.h`，§2.2 也已要求用它构造 clip，属既有依赖）；
  `MarkSurfaceDirty()` 直接 `region16_union_rect(&invalidRegion, &invalidRegion, &rect)`，
  **不再 push 原始矩形**（box 也用不上：`region16_extents()` 就是它）。
- ping-pong 的 damage 账同样用 region16：**在 mark 时**把矩形并进"另一张 picture 的缺失 region"，
  compose 时把两份 region 合并后取 `region16_rects()`；`region16_intersect_rect(region, surfaceRect)`
  代替逐矩形 clamp（与 `gdi_OutputUpdate` 一致）。
- **上限/兜底**：`region16_rects()` 的条数照旧截断（与 CPU 路线的 `kMaxPresentRects` 同口径），
  超限退回 `region16_extents()` 的盒子。
- **预期**：每帧只剩"取列表 + 顺序遍历 + 录 region + 一次 submit"，与 CPU 侧同构；引擎还省掉了
  staging memcpy 与 host→device 上传 ⇒ 应当**持平或反超**。
- **风险/必须验证**：
  - `region16` 在"整屏 Progressive I 帧数千条 mark"下的写入侧开销（gdi 也是这么多，属已证明可接受的
    路径，但要**量**：`presentSplit compose` 必须下降）；
  - region16 是**带 band 的**结构，`region16_rects()` 的输出会**跨越原始矩形之间的间隙**（合并成 band
    内的 bbox）——这正是 gdi 的行为，**照抄即可**，但会让"逐条矩形"的覆盖面积比现在稍大（可接受，
    因为默认是 box）；
  - `REGION16` 与 `Impl::Surface` 一起被容器值拷贝/移动——需要自己写 RAII/移动语义或改成堆持有，
    **不要**直接 memcpy region16（它内部有指针）；
  - 改完必须两份基线录像 `bad=0`，并报 `presentSplit compose` 与 CPU 路线的 `present=` 对比。

### 4.2 解码 kernel 并行化（当前 GPU 路线最大的成本）

解码 kernel 是整帧慢的主因，与上屏无关。设计见
[`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。上屏已不再等解码（§1 的握手），
所以解码变快后上屏也不会变成新的串行段。

### 4.3 整幅重上屏：**已量，判"不做"**

`blit` = 把 picture 整幅 letterbox 写进 swapchain 图像。**swapchain 图像是轮转的**（内容在两次呈现之间
未定义）⇒ 每帧整幅是默认正确做法；要省它只能按 **swapchain 图像**各维护"已写入的增量"（和 §4.1 的
damage 账同一套机制）。**没有** damage-rect 接口可用（`vkQueuePresentKHR` 不给，
`VK_KHR_incremental_present` 不在设备能力表里且只是提示）；**不要**改用
`OH_NativeWindow_*` / `FlushBuffer(..., Region)`。

- **多大**：GPU 时间戳实测 ~1ms/帧、**与脏区无关**（碎片样本同样）；`copy`（脏区字节）另算。
  ⚠ 由 `sync` 墙钟反推的"GPU 十毫秒级"**已证伪**（那是排队）。
- **为什么不做**：这 ~1ms 被 CPU 侧更大的工作量藏住（CPU 侧成本为 0），而实现它要 per-image damage 账，
  且**`bad=0` 覆盖不到呈现器**（compare 路线不 present）⇒ 没有自动化正确性门禁。判据是"降成本、fps 够用"。

### 4.4 其他

- **CPU 路线的上屏已经定型**（主缓冲 = presenter 缓冲 + 脏区恒发 box），细节与约束见
  [`cpu-path.md`](cpu-path.md) §4 —— 包括"掉进 memcpy 就没有的东西别去合并"这条。
- 把 Vulkan 引擎接进 live 会话（现在只有回放/对比跑引擎，live 走 gdi + 呈现器）；
  「硬件解码（RFX）」设置项届时才真正生效。
- 换样本复验：不同分辨率（含宽/高为 64 整数倍）、多条 REGION 的消息；**每份新捕获先自己过 `bad=0`**。

## 5. 操作与踩坑

- **构建/安装/运行**与 **dev 页驱动**：见 [`build-and-verify.md`](build-and-verify.md) §4/§5.1。
  dev 页「路线」按钮循环 CPU → Vulkan → Vulkan对比，**每次点击都会重启回放**。
- **样本**：设备侧读应用 filesDir 下的固定文件名，用 `hdc file send` **覆盖**（`hdc` 不能在该目录新建）；
  本地副本放 gitignore 目录留档。**换样本后记得还原**。
- **判定"跑完"**：轮询回放 stats 文本里的 `(running=0)`（不要固定 sleep）。
- **长期保留的账**（判断该优化哪一段用）：回放 stats 的 `present=` 行；引擎 `Stats()` 的 `presentSplit`
  （拆成 `compose` 录制 / `flush` 提交 / `blit` 全幅上屏，每帧均值）；CPU 路线是 `upload=` 行
  （`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
- **日志**：hilog domain `0xD001`；**每个格式说明符都要写 `%{public}`**，否则打印 `<private>`。
- **坑（改这块前看一遍）**：
  - winpr 的 `synch.h` 把 `CreateSemaphore` 定义成 `CreateSemaphoreA`，会改写本 TU 里
    `api.CreateSemaphore` 之类的 Vulkan 调用 ⇒ 引擎文件里 `#undef CreateSemaphore`；
  - **两套 format 词汇表**：`Impl::format` 是 `VkFormat`，`GfxSurface::format` 是 FreeRDP 的**打包**格式；
    `clear_decompress` 收的是后者（裸写 `format` 会静默解析成前者 ⇒ ClearCodec 大面积失败，
    自查 `Stats()` 的 `unsupported`/`clearUnsup` 必须为 0）；
  - **`CoalesceRects` 原地排序/合并** `dirtyRects`："合并前"要用的东西必须**先拷出来**（否则账目错位、
    静默丢增量，症状是 `bad>0` 但 `maxDelta` 很小、差异呈"某块漏合成"的形状）；
  - **在被测路径里加读回探针会扰动测量**（额外提交/等待会改变失败形状）——定位要用不进入被测路径的
    读法（只读映射、外部对照），不要把探针塞进被测量的那条链；
  - `rfx_compose` 的 `uSwapRb` 是 shader 的 push-constant 布局，**不能删**（只能恒传 0）。
