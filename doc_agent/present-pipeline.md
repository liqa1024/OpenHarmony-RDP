# 上屏管线（present）

本文件是**上屏这一段**（从「本帧要显示哪些像素」到「swapchain / `vkQueuePresentKHR`」）的口径与交接。
GFX 协议框架与 CPU（gdi）链路的成本结构见 [`gfx-engine.md`](gfx-engine.md)；构建与验证见
[`build-and-verify.md`](build-and-verify.md)。

> **范围**：解码与合成走 FreeRDP gdi（live 与回放同一条路），本文件只讲把它出好的桌面帧送上屏。
> 自研 GPU 解码/合成引擎已移除（见 [`gfx-engine.md`](gfx-engine.md) §3）。

## 1. 一套实现、两个后端

`FramePresenter`（`hmrdp_presenter.h`）只有一件事：把已解码的桌面帧放上 XComponent 表面。
后端由 `CreateFramePresenter()` 按**呈现能力**（`VulkanCapabilities::presenterSupported`）与「硬件加速」设置选：

- **Vulkan（默认）**：脏矩形先拷进 host-visible staging 缓冲，再上传进持久桌面图；GPU 用 letterbox quad
  采样上屏。CPU 从不写显示内存。
- **GLES（兜底）**：设备 Vulkan 不能上屏时用（主要是模拟器）。脏矩形用 ES3 + `GL_UNPACK_ROW_LENGTH`
  传进桌面尺寸纹理，letterbox 由 shader 的 swizzle + letterbox viewport 完成。

「硬件加速」开关**只选上屏后端**（关 = GLES 且不碰 Vulkan），**不是**解码后端（解码始终是 gdi）。
判据与 Vulkan 能力见 [`native-libraries.md`](native-libraries.md) §6。

## 2. 帧到屏的路径

- **picture 图**：FreeRDP 的 BGRA 字节序，格式恒为 `B8G8R8A8_UNORM`（`kPictureFormat`）。
  **通道序由图像格式承担**，shader 不做 R/B 交换；两者只由 quad 采样。
- **上屏**：采样 picture 的 letterbox quad（viewport 做 letterbox），写入 swapchain 图像，再
  `vkQueuePresentKHR`。**不要**改用 `OH_NativeWindow_*` / `FlushBuffer(..., Region)`（那是 CPU/native
  生产者路径，生产者照样要填满 buffer，`Region` 只是给合成器的提示）。
- **帧槽**：`kFramesInFlight = 2`：每槽各有 command buffer / fence / image-available 信号量 /
  render-finished 信号量 / staging 缓冲，以及各自的 descriptor set 与时间戳池。
- **CPU 不做任何像素变换**：通道交换、缩放、letterbox 一律在 GPU 侧，上传只做行拷贝。
- **纹理/桌面图重建后首帧强制整幅**，否则其余部分会留空。
- **`presentWait` = 等显示端**：`present` 那一段里 `vkAcquireNextImageKHR`/fence（GLES 是
  `eglSwapBuffers`）的阻塞。`present` 只记录制/上传/提交，两者相加才是 present 的整段墙钟。

## 3. 脏区形状与上传

- **脏区像素进 GPU**：Vulkan 零拷贝路径**无 memcpy**（gdi 直接合成进 presenter 的 host-visible 缓冲）；
  staging 路径把脏矩形拷进 staging 再上传。
- **上传逐条矩形还是包围盒**（由 `usesDesktopBuffer()` 分流，**不是可独立调的旋钮**）：
  - **零拷贝（Vulkan）恒发合并 box**：CPU 侧成本在**条数**（每条 = 一个 copy region + 一次 flush），
    box 恒一条；按 CPU 侧 `present` 量过，box ≤ 逐条（碎片样本 −17%）。
  - **staging（GLES）恒走逐条矩形**：成本在**字节**，box 会多搬数倍；逐条实测把 host→device 字节
    降七成以上、上屏耗时降三成。
  - 两者都按"一次上传 + 多个拷贝区域 + **仍然只画一次 letterbox quad**"实现，**不要**为每个矩形各走一次
    present（那会把整屏 letterbox 画 N 遍）。
- **矩形上限**：单次 present 最多 `kMaxUploadRects` 条逐条矩形；超限退回合并 box（保证命令列表与拷贝区域
  数组有界）。长期账在回放 stats 的 `upload` 行（`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
- **staging 的 flush 只覆盖本帧写入的字节**（`size` = 实际写入量，不是 `VK_WHOLE_SIZE`）：该缓冲会按见过的
  最大脏区增长，整块 flush 会把本帧没碰过的内存一起提交。
- **不要再叠加 present-on-change**：静止态已由 FreeRDP 的失效区门控保证（未执行绘制原语时失效区为空，
  直接返回）——额外 `memcmp` 只增加内存/带宽开销。

## 4. 零拷贝桌面缓冲（Vulkan；不要再动）

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
  它在 gfx-engine §8.1 里记为 `sync`（`BeginDesktopBufferWrite`）：**等 GPU 放开主缓冲**。
  ⚠ 等待缺失的后果是**上屏画面混两帧**，且**只在帧背靠背时**出现；`bad=0` 与它无关。
- **内存类型 `HOST_CACHED` 优先**；非连贯时要 flush，**按脏区合并成一个区间刷**（逐条刷会变成每帧上百次
  驱动调用；cache flush 只写回脏行，多出来的干净行免费）。
- **几何变化**（`ResetGraphics` → `gdi_resize`）会退回 gdi 自有缓冲，下一次 `EndPaint` 按新尺寸重挂。
- 前提：**进程内只建一个 `VkDevice`**。
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

## 4.5 超分辨率（Vulkan 专有）

超分辨率（平台文档称「超分」）让会话按**更低的「会话分辨率」**取流，本机再把每帧上采样回设置里的
**「输出分辨率」**：省的是解码与带宽（服务端少出像素），换来的是边缘更锐的本地放大。倍率与默认缩放见
[`settings-and-storage.md`](settings-and-storage.md) §2；能力判定见
[`native-libraries.md`](native-libraries.md) §6。

- **只有 Vulkan 呈现器有**：GLES 是兼容兜底，不做同样功能（`SetSuperResolution()` 的默认实现是空操作，
  只有 `VkRenderer` 覆盖它）。因此超分辨率要求「硬件加速」开着，否则会话侧根本不会降分辨率。
- **两张图**：输入仍是桌面图（`kPictureFormat` = `B8G8R8A8_UNORM`，会话分辨率），输出是新增的
  **输出图**（同格式、`USAGE = COLOR_ATTACHMENT | SAMPLED`、device-local、输出分辨率）。通道序仍由
  图像格式承担，shader 不做交换。letterbox 改成**采样输出图**，源尺寸取输出分辨率（再 fit 到窗口）。
- **屏障**：输出图每帧被整幅重写，所以进 pass 前用 `UNDEFINED → GENERAL`（丢弃旧内容，免去布局追踪），
  出 pass 后用 `COLOR_ATTACHMENT_WRITE → SHADER_READ`；桌面图到上采样输入的依赖沿用原来那条
  `TRANSFER_WRITE → SHADER_READ` 屏障。
- **生命周期**：上采样对象与输出图按「输入尺寸 + 输出尺寸」缓存，尺寸变了才重建；重建点放在
  **acquire 之前**（`EnsureDesktopImageLocked` 之后），这样重建里的 fence 等待不会与
  `AcquireFrameLocked`（它已 reset 本槽 fence）互锁。桌面图重建会一并拆掉超分辨率资源。
- **失败就降级且不再重试**（`srFailed_` 锁存）：设备拒绝该尺寸/格式（或库缺失）时不进 pass，直接按桌面图
  letterbox，会话照常可用。排查看 hilog 的 `xengine: spatial upscale` 与
  `vulkan presenter: super resolution`。

## 5. swapchain 的尺寸与重建（Vulkan）

- 尺寸以 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 的 `currentExtent` 为准，只有在它是 `UINT32_MAX`
  时才用窗口尺寸 clamp。
- present 返回 `VK_ERROR_OUT_OF_DATE_KHR` 就重建后跳过该帧、`VK_SUBOPTIMAL_KHR` 则先呈现再重建。
- **重建只重建"依赖变了的资源"**——swapchain/图像/视图/framebuffer/每图像信号量；render pass 只依赖
  **格式**，command pool/buffer 与每帧 fence/信号量都与尺寸无关，都要保留；旧 swapchain 通过
  `oldSwapchain` 交回驱动复用。
- 窗口尺寸变化**不是**故障，也不要为"等尺寸稳定"去延迟创建或主动重建。

## 6. 账目与探针

- **`present=`**：绝大部分是**固定的 Vulkan 调用**（acquire / 录制 / submit / present），不是能删的活；
  `flush` 只有 µs 级。`presentWait`（阻塞）单列，见 §2。
- **GPU 侧另算**：时间戳查询（每槽 4 个）把一次提交切成 **`copy`（脏区字节 buffer→image）/
  `sr`（超分辨率上采样 pass）/ `blit`（clear 整张 + letterbox quad）**三段；超分辨率关闭时 `sr` 落在
  屏障之后、自然接近 0。三个数进遥测（`gpuCopyUs`/`gpuSrUs`/`gpuBlitUs`，见
  [`session-and-input.md`](session-and-input.md) §3），另有一条 30 帧一打的 hilog。
  **它们只是"本机这一帧让 GPU 干了多久"**：GPU 整机占用率没有对三方应用开放的接口。
  **`blit` 与脏区无关**：swapchain 图像是轮转的（内容在两次呈现之间未定义）⇒ 每帧整幅是默认正确做法；
  要省它只能按 **swapchain 图像**各维护"已写入的增量"。没有 damage-rect 接口可用
  （`vkQueuePresentKHR` 不给，`VK_KHR_incremental_present` 不在设备能力表里且只是提示）；
  **不要**改用 `OH_NativeWindow_*` / `FlushBuffer(..., Region)`。
  - **多大**：GPU 时间戳实测 ~1ms/帧、**与脏区无关**（碎片样本同样）；`copy`（脏区字节）另算。
    ⚠ 由 `sync` 墙钟反推的"GPU 十毫秒级"**已证伪**（那是排队）。
  - **为什么不做**：这 ~1ms 被 CPU 侧更大的工作量藏住，而实现它要 per-image damage 账，
    且**`bad=0` 覆盖不到呈现器**（compare 路线不 present）⇒ 没有自动化正确性门禁。判据是"降成本、fps 够用"。
- **零拷贝下 `copy` 与帧序是耦合的**：它读的就是本帧要写的那块内存，所以排在**本帧第一次写之前**
  （`sync`）。脏区字节大时它直接进帧墙钟。判据：**`sync` 与 `uploaded` 同步起落**就是这一项，
  而不是"合成贵"。

## 7. 操作与踩坑

- **构建/安装/运行**与 **dev 页驱动**：见 [`build-and-verify.md`](build-and-verify.md) §4/§5.1。
  dev 页「路线」只切 CPU / 硬件加速（上屏后端），「参考」切 关/导出/对比，**每次点击都会重启回放**。
- **样本**：设备侧读应用 filesDir 下的固定文件名，用 `hdc file send` **覆盖**（`hdc` 不能在该目录新建）；
  本地副本放 gitignore 目录留档。**换样本后记得还原**。
- **判定"跑完"**：轮询回放 stats 文本里的 `(running=0)`（不要固定 sleep）。
- **长期保留的账**：回放 stats 的 `present=` 行与 `upload=` 行
  （`uploaded=` / `box=` / `rectlist=` / `truncated=`）。
- **日志**：hilog domain `0xD001`；**每个格式说明符都要写 `%{public}`**，否则打印 `<private>`。
- **坑（改这块前看一遍）**：
  - **在被测路径里加读回探针会扰动测量**（额外提交/等待会改变失败形状）——定位要用不进入被测路径的
    读法（只读映射、外部对照），不要把探针塞进被测量的那条链；
  - 呈现器选择在**创建时**决定（`CreateFramePresenter`），所以改「硬件加速」要重启会话/回放才生效。
