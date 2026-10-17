# RDP 上屏接线（脏区上传 / 零拷贝 / 合并上屏）

本文件是 **RDP（gdi）侧上屏接线**的家：桌面帧怎么带脏区喂给共享呈现器、零拷贝的等待与顺序约束，
以及由此而来的账目与操作坑。共享的上屏管线（一套 `FramePresenter`、Vulkan/GLES 两个后端、swapchain 尺寸与
重建、超分辨率、GPU 账目、呈现与超分能力判定）见 [`present.md`](present.md)；GFX 协议框架与 CPU（gdi）链路的
成本结构见 [`rdp-gfx.md`](rdp-gfx.md)。

## 1. 脏区形状与上传

- **脏区像素进 GPU**：Vulkan 零拷贝路径**无 memcpy**（gdi 直接合成进 presenter 的 host-visible 缓冲）；
  staging 路径把脏矩形拷进 staging 再上传。
- **上传逐条矩形还是包围盒**（由 `usesDesktopBuffer()` 分流，**不是可独立调的旋钮**）：
  - **零拷贝（Vulkan）恒发合并 box**：CPU 侧成本在**条数**（每条 = 一个 copy region + 一次 flush），
    box 恒一条，`present` 成本 ≤ 逐条（碎片样本更明显）。
  - **staging（GLES）恒走逐条矩形**：成本在**字节**，box 会多搬数倍；逐条把 host→device 字节降到
    一个量级以下、上屏耗时降约三分之一。
  - 两者都按"一次上传 + 多个拷贝区域 + **仍然只画一次 letterbox quad**"实现，**不要**为每个矩形各走一次
    present（那会把整屏 letterbox 画 N 遍）。
- **矩形上限**：单次 present 最多 `kMaxPresentRects` 条逐条矩形（`hmrdp_gfx_cpu.cpp`，与呈现器内部的
  `kMaxUploadRects` 数组上界同值），超限退回合并 box，保证命令列表与拷贝区域数组有界。长期账在回放
  stats 的 `upload` 行（`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
- **staging 的 flush 只覆盖本帧写入的字节**（`size` = 实际写入量，不是 `VK_WHOLE_SIZE`）：该缓冲会按见过的
  最大脏区增长，整块 flush 会把本帧没碰过的内存一起提交。
- **不要再叠加 present-on-change**：静止态已由 FreeRDP 的失效区门控保证（未执行绘制原语时失效区为空，
  直接返回）——额外 `memcmp` 只增加内存/带宽开销。

## 2. 零拷贝桌面缓冲（Vulkan；不要再动）

gdi 直接合成进 presenter 拥有的 host-visible 缓冲，present 只做一次 buffer→image 拷贝，**没有帧拷贝**。

- **挂接**：呈现器**每帧问一次、成功为止**（Vulkan 设备随 surface 建，`gdi_init` 时可能还没有），
  live 与回放共用 `InitGdiWithPresenter` / `AttachPresenterDesktopBuffer`。
  **挂接不要用 `gdi_resize_ex`**（它会再调 `update_end_paint`，把 `update->mux` 的配对搞乱）；就地换
  `primary->bitmap` 的 data/scanline/free 与 `gdi->stride`，**换前把已合成的桌面拷过去**（gdi 图元会读
  目标缓冲）。
- **单缓冲 ⇒ 本帧第一次写之前必须等上一帧的 GPU 读完**。这是**顺序约束**：等待点取在
  `update->BeginPaint` 是错的（那是 `gdi_OutputUpdate` 里、整帧解码之后发的），正确位置是**每个可能写
  桌面的命令之前**（GFX 帧边界与表面命令，以及 SolidFill / SurfaceToSurface / 缓存 / ResetGraphics 这类
  结构命令——**帧外**同样会来表面命令），挂在 `GfxWorkSetFrameBeginHook`；等待按"提交"去重
  （`desktopBufferFencePending_`）⇒ **每个 present 只真等一次**。
  它在 [`rdp-gfx.md`](rdp-gfx.md) §7.1 里记为 `sync`（`BeginDesktopBufferWrite`）：**等 GPU 放开主缓冲**。
  ⚠ 等待缺失的后果是**上屏画面混两帧**，且**只在帧背靠背时**出现；`bad=0` 与它无关。
- **内存类型 `HOST_CACHED` 优先**；非连贯时要 flush，**按脏区合并成一个区间刷**（逐条刷会变成每帧上百次
  驱动调用；cache flush 只写回脏行，多出来的干净行免费）。
- **几何变化**（`ResetGraphics` → `gdi_resize`）会退回 gdi 自有缓冲，下一次 `EndPaint` 按新尺寸重挂。
- 前提：**桌面缓冲归它所在 presenter 的 `VkDevice`**（每 presenter 一个 device，进程内可有多个，
  互不共享）。
- **桌面镜像 surface 直接合成进 primary**（全屏 GFX 会话的常态：只有一个 surface、`(0,0)` 1:1、格式/行距
  与桌面相同 ⇒ 它**就是桌面**，那趟逐矩形 `freerdp_image_scale` 纯属白搬）。做法是把这个 surface 的 `data`
  指向 `gdi->primary_buffer`。**生命周期是重点**：
  - 凭证就是 `surface->data == gdi->primary_buffer`；任何一条不满足就退回逐矩形拷贝。
  - `gdi_ResetGraphics` **保留** surface 并 memset 它，而它调的 `DesktopResize` 会**换掉 primary** ⇒ 必须
    **换之前**记下谁在共享、换之后重指向或让它自己分配。
  - `gdi_DeleteSurface` 不能释放共享缓冲；出现**第二个** surface 时先解除共享。
  - ⚠ **gfx 上下文拆卸也要经 `DeleteSurface` 走一遍**（`rdpgfx_client_context_free` → `free_surfaces`），
    而 `gdi_DeleteSurface` 从 `context->custom` 取 gdi——`gdi_graphics_pipeline_uninit` 已经把它清空 ⇒
    共享测试必然失败、把**呈现器的 host-visible 缓冲**当自有缓冲 `winpr_aligned_free`（进程直接挂）。
    **顺序**：谁拆 gfx 上下文，谁就必须**在 uninit 之前**先 `DeleteSurface` 掉所有 surface（离线 CPU 桌面
    就是这么做）；live 靠 FreeRDP 在 `OnClose` 里先 `free_surfaces`。
  - 全在 `libfreerdp/gdi/gfx.c` 内部，**不动头文件也不动 app**，因此对任何呈现器都成立。

## 3. 合并上屏（送显跟不上时不丢帧）

桌面是**状态流**（RDP GFX 帧是增量，丢一帧就是一处永久错像素），所以本机刷新率不够时（省电档、档位上不来）
**不丢帧、也不硬顶**，而是**一个刷新周期只送一次**：周期内到达的帧把脏区并进"待送区"，由下一次送显一起带出
（面板档位与送显槽位取自共享的 `hmrdp_display_rate.*`，机制见 [`present.md`](present.md) §2）。合成照旧
逐帧做，所以屏上永远是**最新状态**，也不会为看不见的帧白做 `blit`。**待送区必须有人兜底**：GFX 工作线程在
队空时等它自己的槽位再送（帧到达则先合成），所以一串帧的最后一帧不会悬在那里。回放页**不开**这套（它要
逐帧确定性）；视频链路的同类判断是**丢弃**而不是合并（[`moonlight-video.md`](moonlight-video.md) §1）。

## 4. 账目与探针（RDP 侧）

- **合并上屏后 `present` 记在真正送出的那一帧**：被并进来的帧只有合成、没有 `present`（像素由下一次一起传），
  所以按帧读会跳、按窗口读不变。
- **`sync` = 等 GPU 放开主缓冲**（`BeginDesktopBufferWrite`；CPU 路线让解码器直接写呈现器缓冲，所以**本帧
  第一次写之前**要等上一帧的 GPU 拷贝读完，等待点见 §2）。它**是阻塞、不入 `本机`**（口径见
  [`rdp-gfx.md`](rdp-gfx.md) §7.1）。
- **零拷贝下 `copy` 与帧序是耦合的**：它读的就是本帧要写的那块内存，所以排在**本帧第一次写之前**（`sync`）。
  脏区字节大时它直接进帧墙钟。判据：**`sync` 与 `uploaded` 同步起落**就是这一项，而不是"合成贵"。
- **长期保留的账**：回放 stats 的 `present=` 行与 `upload=` 行（`uploaded=` / `box=` / `rectlist=` / `truncated=`）。

## 5. 操作与踩坑

- **构建/安装/运行**与 **dev 页驱动**：见 [`build-and-verify.md`](build-and-verify.md) §4/§5.1。
  dev 页「路线」只切 CPU / 硬件加速（上屏后端），「参考」切 关/导出/对比，**每次点击都会重启回放**。
- **样本**：设备侧读应用 filesDir 下的固定文件名，用 `hdc file send` **覆盖**（`hdc` 不能在该目录新建）；
  本地副本放 gitignore 目录留档。**换样本后记得还原**。
- **判定"跑完"、驱动与多轮测量**：见 [`rdp-gfx.md`](rdp-gfx.md) §5 与
  [`build-and-verify.md`](build-and-verify.md) §5.2。
- **日志**：hilog domain `0xD001`；**每个格式说明符都要写 `%{public}`**，否则打印 `<private>`。
- **坑（改这块前看一遍）**：
  - **在被测路径里加读回探针会扰动测量**（额外提交/等待会改变失败形状）——定位要用不进入被测路径的
    读法（只读映射、外部对照），不要把探针塞进被测量的那条链；
  - 呈现器选择在**创建时**决定（`CreateFramePresenter`），所以改「硬件加速」要重启会话/回放才生效。
