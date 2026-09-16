# 上屏管线（present）与后续工作

本文件是**上屏这一段**（从「本帧要显示哪些像素」到「swapchain / `vkQueuePresentKHR`」）的口径与**交接**。
协议/解码语义见 [`gfx-engine.md`](gfx-engine.md)（§0.4、§2.3、§6、§7），构建与验证见
[`build-and-verify.md`](build-and-verify.md)。

> 前提：**GPU 引擎与 GPU 回放只在真机验证**；模拟器上的 Vulkan 会按标准接口谎报能力，不在支持范围。

## 1. 目标是"两条路线共用一套上屏实现，并且 GPU 侧不能比 CPU 侧慢"

CPU 路线（gdi）与 GPU 路线（引擎）**共用同一个 `VkRenderer`**，且**只有一条上屏实现**：

- **picture 图**： FreeRDP 的 BGRA 字节序，格式恒为 `B8G8R8A8_UNORM`（`kPictureFormat`）。生产者可以是
  CPU 帧（脏区 memcpy 进 staging 再拷）或引擎（表面 → picture 的**设备内**拷贝）；**通道序由图像格式承担**，
  shader 不做 R/B 交换（见 gfx-engine.md §2.3）。
- **上屏**：采样 picture 的 letterbox quad（`RecordPresentQuadLocked`，viewport 做 letterbox），
  写入 swapchain 图像，再 `vkQueuePresentKHR`。**不要**改用 `OH_NativeWindow_*`/`FlushBuffer(..., Region)`
  （那是 CPU/native 生产者路径，生产者照样要填满 buffer，`Region` 只是给合成器的提示；本仓库已试过并删除）。
- **帧槽**：引擎 `kSlots = 2`：每槽各有 command buffer / fence / frame semaphore / engine-chain semaphore /
  blit-done semaphore，以及 staging arena、descriptor pool、时间戳池、延迟回收列表、"compute 在飞"标志
  **各一份**（`FlushAll()` 供 CPU 读回路径等所有在飞槽）。
- **picture ping-pong**：每个槽一张 picture；每张带"另一张拿到而自己错过的矩形"账
  （`pictureMissing[图][surface]`），compose 时写 `本帧脏区 ∪ 该图错过的脏区`，随后清空自己、把本帧脏区交给另一张。
  **账上只存矩形、像素一律从当前表面拷**（表面已是最新）。
- **设备侧握手（CPU 不等待）**：
  - `engineChain[slot]`：每次提交都 signal、**下一次提交** wait —— surfaces 与解码 scratch 跨帧读改写，
    帧间顺序由它保证（这是必须的，不要用"等上一次 blit"来代替，那会把 compose 与 blit 也串起来）；
  - `frameSemaphores[slot]`：帧提交 signal、该帧的 blit wait；
  - `blitDone[slot]`：引擎交给 presenter 的令牌，presenter 在 blit 结束时 signal，**两帧后**这张 picture
    被重写前才 wait（picture 复用的保护）。present 失败必须 `AbandonBlitDoneHandoff()`，否则令牌永不被 signal。
  - CPU 读回路径（`ReadScreen`、ClearCodec/cache 的 RMW、`Reset`/`ResetGraphics`）走 `FlushAll()` 等所有在飞槽。

## 2. 现状：上屏耗时已经接近 CPU，但还差 +0.73ms/帧

同一会话、同一份滑动样本、`mode=fast`（回放页「Vulkan对比」）：

| 每帧 | CPU 路线 | GPU 路线 |
|---|---|---|
| `present=` 合计 | **2.61 ms** | **3.34 ms** |
| 脏区负载 | 2.3 MB/帧 **主机→device** 上传（staging memcpy + N 个 copy region） | **设备内**拷贝（无 memcpy） |
| 脏区记账/录制 | 不用 | `compose = 488 µs` |
| 提交 | 含在上面 | `flush = 334 µs` |
| letterbox quad + present | 同一份代码 | `blit = 2516 µs`（含 acquire/描述符/录制/提交/present） |
| 是否等本帧 GPU | 不等 | 不等 |
| 整帧（参考） | `fps 50.0` / `feed 7678ms` | `fps 16.4` / `feed 37139ms`（对照路线还多跑一遍 gdi 解码） |

历史口径：这轮改造**之前** GPU 的 `present` 是 **13.75 ms（CPU 的 5.3 倍）**，因为它在 present 里
`Flush()`（submit **+** wait）等整帧解码；`presentSplit` 的 `flush` 桶 11507 → 334 µs 记录了这条等待的消失。
整帧仍差与上屏无关（解码 kernel ≈17ms/帧 + 引擎 CPU 侧 PDU 处理）。

## 3. 那 0.73ms 的来源（代码级）——**这是下一步要优化的东西**

**CPU 侧为什么不需要这笔计算**：gdi 的脏区是**增量维护**的。每条命令写入时
`gdi_InvalidateRegion(hdc, x, y, w, h)` 把矩形并进该表面**持久的 `invalidRegion`（region16）**；
帧末 `gdi_StartFrame/EndFrame` 才 `gdi_UpdateSurfaces` → `gdi_OutputUpdate`：
`region16_intersect_rect(invalidRegion, surfaceRect)`（裁到 mapped 尺寸）→ `region16_rects()` 取
**已合并、已裁剪**的矩形表 → 按矩形 memcpy 进 staging + 录 N 个 `VkBufferImageCopy`。
**合并发生在写入那一刻、用的还是它本来就需要的结构**，每帧只剩一次"取列表 + 顺序遍历"。

**GPU 侧现在是怎么写的**（`GfxVkDesktop::Impl`）：脏区是

1. `std::vector<Surface::DirtyRect> dirtyRects`（**原始 mark**，每次写入 push 一条，上限 8192；
   Progressive 一条消息按**每个解码 tile** 各推一条 ⇒ 整屏 I 帧可达 ~2900 条）；
2. 外加一份**并行的 union box**（`meta.dirtyLeft/Top/Right/Bottom`），只为"矩形太长时退回盒子"服务。

每帧 `Compose()` 对每个表面做（同一份信息被扫 3 遍 + 一次排序）：

1. **`CoalesceRects()`**：对全部原始 mark **排序**（`std::sort`）再两趟合并；**box 模式下这个结果只用来
   统计与兜底判断，并不需要列表本身**；
2. 读 `meta.dirty*` 作为兑现盒；
3. 逐矩形 `MakeScreenCopyRegion()`：clamp + 计算 dst + `MarkScreenDirty()`；
4. **ping-pong 的 damage 账**：`pictureMissing[pic][surface]` 的 append/clear + 缺失矩形并集（还要在
   box 模式下把缺失矩形的 hull 并进盒子）。

⇒ 这 0.49 ms 是**记账与录制**，不是带宽（`flush` 只有 334 µs；ping-pong 让字节翻倍但字节便宜）。

## 4. 待办（按收益/风险排序）

### 4.1 用"增量 region16"重写引擎的脏区账（目标：上屏 ≤ CPU）

**思路**：把 §3 的 1/2/4 换成 gdi 那套——**在写入时增量合并，而不是每帧重算**：

- `Surface` 持有一个 **`REGION16 invalidRegion`**（FreeRDP 的 region API；引擎已 include
  `freerdp/codec/region.h`，且 §2.2 已经要求用它的 `region16_union_rect` 构造 clip，属既有依赖）；
  `MarkSurfaceDirty()` 直接 `region16_union_rect(&invalidRegion, &invalidRegion, &rect)`，
  **不再 push 原始矩形**（box 也用不上：`region16_extents()` 就是它）。
- ping-pong 的 damage 账同样用 region16：**在 mark 时**把矩形并进"另一张 picture 的缺失 region"，
  compose 时把两份 region 合并后取 `region16_rects()`；`region16_intersect_rect(region, surfaceRect)`
  代替逐矩形 clamp（与 `gdi_OutputUpdate` 一致）。
- **上限/兜底**：`region16_rects()` 的条数照旧按 256 截断（CPU 路线的 `kMaxPresentRects` 也是 256，
  本样本 `truncated=1`，两边口径一致），超限退回 `region16_extents()` 的盒子。
- **预期**：每帧只剩"取列表 + 顺序遍历 + 录 region + 一次 submit"，与 CPU 侧同构；引擎还省掉了
  staging memcpy（2.3MB/帧）与 host→device 上传 ⇒ 应当**持平或反超**。
- **风险/必须验证**：
  - `region16` 在"整屏 Progressive I 帧 ~2900 条 mark"下的写入侧开销（gdi 每帧也是这么多，
    属已证明可接受的路径，但要**量**：`presentSplit compose` 必须下降）；
  - region16 是**带 band 的**结构，`region16_rects()` 的输出会**跨越原始矩形之间的间隙**（合并成 band 内的
    bbox）——这正是 gdi 的行为（§2.2 已记录），**照抄即可**，但会让"逐条矩形"的覆盖面积比现在稍大（可接受，
    因为默认是 box）；
  - `REGION16` 与 `Impl::Surface` 一起被 `std::map` 值拷贝/移动（`surfaces[id] = surface`）——需要自己写
    RAII/移动语义或改成堆持有，**不要**直接 memcpy region16（它内部有指针）；
  - 改完必须两份录像 `bad=0`（滚动 `.cache/hmrdp_gfx.bin`、视频 `.cache/hmrdp_gfx_video.bin`），
    并报 `presentSplit compose` 与 CPU 路线的 `present=` 对比。

### 4.2 解码 kernel 并行化（当前 GPU 路线最大的成本）

`gpuMs rlgr + idwt ≈ 17ms/帧`（视频样本 ≈190ms/帧），是整帧慢的主因，与上屏无关。
设计见 [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。上屏已不再等解码（§1 的握手），
所以解码变快后上屏也不会变成新的串行段。

### 4.3 把上屏的整幅 quad 缩成脏矩形（≈2.8%，排在解码之后）

`blit` 桶里包含"把 picture 整幅 letterbox 写进 swapchain 图像"。swapchain 图像是**轮转**的（内容在两次
呈现之间未定义），所以每帧整幅是**默认正确做法**；要省它只能按 **swapchain 图像**各维护"已写入的增量"
（每张图补它错过的 delta）——与 §4.1 的 damage 账是同一套机制。`vkQueuePresentKHR` 这条路**没有**
damage-rect 接口，`VK_KHR_incremental_present` 也不在设备能力列表里（且它只是提示、不减少我们自己的写入）；
**不要**改用 `OH_NativeWindow_*`/`FlushBuffer(..., Region)`（CPU/native 生产者路径，已删除）。

### 4.4 其他

- **CPU 路线：让主缓冲就是 staging buffer（去掉"primary→staging"那一遍 20.6MB/帧的 memcpy）**，
  以及**逐矩形上传前先把同带相邻矩形并成更长的条**（滚动样本 present 2.5ms 只搬 2.2MB，碎矩形
  的行拷贝 + copy region 数是主因）：依据、配到的口径与风险见
  [`cpu-path.md`](cpu-path.md) §6.1/§6.2。
- 把 Vulkan 引擎接进 live 会话（现在只有回放/对比跑引擎，live 走 gdi + 呈现器）；
  「硬件解码（RFX）」设置项届时才真正生效。
- 换样本复验：不同分辨率（含宽/高为 64 整数倍）、多条 REGION 的消息；**每份新捕获先自己过 `bad=0`**。

## 5. 操作与踩坑（下一个会话直接用）

- **构建/安装/运行**：`devecocli build`（arm64）→ `native/scripts/install-device.ps1 -Device "<序列号>"`；
  dev 页「回放测试」的「路线」按钮循环 CPU → Vulkan → Vulkan对比（**每次点击都会重启回放**）。
- **样本**：设备侧固定路径 `…/files/hmrdp_gfx.bin`，用 `hdc file send` **覆盖**（`hdc` 不能在该目录新建）。
  本地留档 `.cache/hmrdp_gfx.bin`（滚动/碎片）、`.cache/hmrdp_gfx_video.bin`（视频）。**换样本后记得还原**。
- **判定"跑完"**：轮询回放 stats 文本里的 `(running=0)`（不要固定 sleep）。
- **长期保留的账**（判断该优化哪一段用）：回放 stats 的 `present=` 行；
  引擎 `Stats()` 的 `presentSplit`（present 拆成 `compose` 录制 / `flush` 提交 / `blit` 全幅上屏，每帧均值）；
  CPU 路线是 `upload=` 行（`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
- **日志**：hilog domain `0xD001`；**每个格式说明符都要写 `%{public}`**，否则打印 `<private>`。
- **坑（都踩过，改这块前看一遍）**：
  - winpr 的 `synch.h` 把 `CreateSemaphore` 定义成 `CreateSemaphoreA`，会改写本 TU 里 `api.CreateSemaphore`
    之类的 Vulkan 调用 ⇒ 引擎文件里 `#undef CreateSemaphore`；
  - **两套 format 词汇表**：`Impl::format` 是 `VkFormat`，`GfxSurface::format` 是 FreeRDP 的**打包**格式；
    `clear_decompress` 收的是后者（裸写 `format` 会静默解析成前者 ⇒ ClearCodec 大面积失败，
    自查 `Stats()` 的 `unsupported`/`clearUnsup` 必须为 0）；
  - **`CoalesceRects` 原地排序/合并** `dirtyRects`："合并前"要用的东西必须**先拷出来**（否则账目错位、
    静默丢增量，症状是 `bad>0` 但 `maxDelta` 很小、差异呈"某块漏合成"的形状）；
  - **在被测路径里加读回探针会扰动测量**（额外提交/等待会改变失败形状）——定位要用不进入被测路径的
    读法（只读映射、外部对照），不要把探针塞进被测量的那条链；
  - `rfx_compose` 的 `uSwapRb` 是 shader 的 push-constant 布局，**不能删**（只能恒传 0）。
