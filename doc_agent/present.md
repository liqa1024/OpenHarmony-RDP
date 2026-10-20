# 上屏管线（present）

本文件是**上屏这一段**（从「本帧要显示哪些像素」到 `swapchain` / `vkQueuePresentKHR`）的家，**两协议共享**。
RDP（gdi）侧特有的脏区上传、零拷贝桌面缓冲、合并上屏与由此而来的账目见 [`rdp-present.md`](rdp-present.md)；
GFX 协议框架与 CPU（gdi）链路的成本结构见 [`rdp-gfx.md`](rdp-gfx.md)；Moonlight 的两条上屏链路见
[`moonlight-video.md`](moonlight-video.md)；构建见 [`build.md`](build.md)，测试与验证见 [`testing.md`](testing.md)。

> **范围**：RDP 的解码与合成走 FreeRDP gdi（live 与回放同一条路），Moonlight 直出时不经呈现器、超分时把
> 解码帧交给同一呈现器；本文件只讲把出好的帧送上屏。自研 GPU 解码/合成引擎**不采用**（见
> [`rdp-gfx.md`](rdp-gfx.md) §3）。

## 1. 一套实现、两个后端

`FramePresenter`（`hmrdp_presenter.h`）只有一件事：把一帧画面放上 XComponent 表面。桌面帧由 gdi 出，
Moonlight 的 超分链路把解码出的 NV12 帧交给它（见 §4、[`moonlight-video.md`](moonlight-video.md) §2）。
后端由 `CreateFramePresenter()` 按**呈现能力**（`VulkanCapabilities::presenterSupported`）自动选，没有用户开关
（dev 回放页可临时把它压到 GLES 做 A/B，会话开始时按能力再次归一）：

- **Vulkan（默认）**：脏矩形先拷进 host-visible staging 缓冲，再上传进持久桌面图；GPU 用 letterbox quad
  采样上屏。CPU 从不写显示内存。
- **GLES（兜底）**：设备 Vulkan 不能上屏时用（主要是模拟器）。脏矩形用 ES3 + `GL_UNPACK_ROW_LENGTH`
  传进桌面尺寸纹理，letterbox 由 shader 的 swizzle + letterbox viewport 完成。

后端选择**只决定上屏走哪条 GPU 路径**，**不是**解码后端（RDP 解码始终是 gdi）。能力判定见 §5。

**GPU 归属**：进程只共享不可变的 `VkInstance`；**每个 presenter 自带 `VkDevice` + queue**（GLES 同理，
只共享 EGL display，context/surface 每实例独立），所以多会话不会共用一条提交队列、也不会互相拆
device（[`architecture.md`](architecture.md) §4.1）。

## 2. 帧到屏的路径

- **会话 surface 就是内容比例**：会话页用 `components/SessionPicture` 把 surface 约束成桌面/输出分辨率比例
  并居中，所以这里对会话的 letterbox 恒为恒等（等比例时 `offX/offY=0`、不额外做事）。这段等比代码是给
  **回放页**（`GfxReplayPage`）留的——它把抓帧塞进任意比例的格子，依赖呈现器按比例 fit。
- **picture 图**：FreeRDP 的 BGRA 字节序，格式恒为 `B8G8R8A8_UNORM`（`kPictureFormat`）。
  **通道序由图像格式承担**，shader 不做 R/B 交换；两者只由 quad 采样。
- **上屏**：采样 picture 的 letterbox quad（viewport 做 letterbox），写入 swapchain 图像，再
  `vkQueuePresentKHR`。**不要**改用 `OH_NativeWindow_*` / `FlushBuffer(..., Region)`（那是 CPU/native
  生产者路径，生产者照样要填满 buffer，`Region` 只是给合成器的提示）。
- **帧槽**：`kFramesInFlight = 2`：每槽各有 command buffer / fence / image-available 信号量 /
  staging 缓冲，以及各自的 descriptor set 与时间戳池。render-finished 信号量按 **swapchain 图像**
  各一个（不是按帧槽）——它必须等对应图像被再次 acquire 后才能复用。
- **CPU 不做任何像素变换**：通道交换、缩放、letterbox 一律在 GPU 侧，上传只做行拷贝。
- **纹理/桌面图重建后首帧强制整幅**，否则其余部分会留空。
- **`presentWait` = 等显示端**：`present` 里 `vkAcquireNextImageKHR`/fence（GLES 是 `eglSwapBuffers`）的阻塞。
  **单列、不入 `本机`**（定义与自检见 [`rdp-gfx.md`](rdp-gfx.md) §7.1）。
- **送显槽位（共享机制）**：本机刷新率不够时（省电档、档位上不来）的送显节拍由共享的
  `hmrdp_display_rate.*` 给出（面板档位 + "一个周期一帧"的槽位游标）。两条链路对多余帧的处置不同：
  桌面是**状态流**（丢一帧就是一处永久错像素）⇒ **合并**（[`rdp-present.md`](rdp-present.md) §3）；
  视频是**流**（丢帧无害）⇒ **丢弃**（[`moonlight-video.md`](moonlight-video.md) §1）。
- **档位取"会话窗口所在显示设备"**：只有 UI 层知道窗口落在哪块屏（`window` 属性 → `display` 的
  `refreshRate`），所以由它把该档位推给原生（`setDisplayRefreshRateHz`，见 `services/SessionDisplayRate`），
  在显示变化与窗口跨屏后重推；原生在没有推送时回落到**默认显示设备**的自动读数（首次读 + 变化监听）。
  多屏 2in1 下"默认屏 ≠ 会话屏"会让槽位判据与刷新率归因一起偏，故一律以窗口所在屏为准。`DisplayBelowFrameRate()`
  与槽位游标读的是同一个值。

## 3. swapchain 的尺寸与重建（Vulkan）

- 尺寸以 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 的 `currentExtent` 为准，只有在它是 `UINT32_MAX`
  时才用窗口尺寸 clamp。
- present 返回 `VK_ERROR_OUT_OF_DATE_KHR` 就重建后跳过该帧、`VK_SUBOPTIMAL_KHR` 则先呈现再重建。
- **重建只重建"依赖变了的资源"**——swapchain/图像/视图/framebuffer/每图像信号量；render pass 只依赖
  **格式**，command pool/buffer 与每帧 fence/信号量都与尺寸无关，都要保留；旧 swapchain 通过
  `oldSwapchain` 交回驱动复用。
- 窗口尺寸变化**不是**故障，也不要为"等尺寸稳定"去延迟创建或主动重建。
- **letterbox / 上屏的目标取 surface 的真实像素尺寸**，不取窗口生命周期上报的尺寸：窗口不可见时系统会重新
  分配 surface 并可能先报一个过渡尺寸，按上报值拟合会把画面缩放到已经不存在的尺寸。Vulkan 取重建自
  `currentExtent` 的尺寸；GLES 每次上屏用 `eglQuerySurface` 取真实尺寸。
- **尺寸没变不重建**：`ResizeSurface` 收到与当前相同的尺寸直接返回；重复事件置脏只会白白拆建一次
  swapchain，观感是一次重拟合。

## 4. 超分辨率（Vulkan 专有）

超分辨率（平台文档称「超分」）让会话按**更低的「会话分辨率」**取流，本机再把每帧上采样回设置里的
**「输出分辨率」**：省的是解码与带宽（服务端少出像素），换来的是边缘更锐的本地放大。倍率与默认缩放见
[`settings-and-storage.md`](settings-and-storage.md) §2；能力判定见 §5。

- **只有 Vulkan 呈现器有**：GLES 是兼容兜底，不做同样功能（`SetSuperResolution()` 的默认实现是空操作，
  只有 `VkRenderer` 覆盖它）。因此超分辨率要求设备呈现判定成立（Vulkan 上屏），否则会话侧根本不会降分辨率。
- **两个后端，一套接线**：`SetSuperResolution(enabled, backend, outW, outH)` 只选上采样器——
  **XEngine**（平台空域超分）或 **FSR 1.0**（呈现器自己跑的 EASU + RCAS，MIT）。两者都写进输出分辨率的
  `srImage_`，其后的 letterbox 完全相同，所以后端只是录制方式不同；后端在呈现器创建时定，换后端要重连会话。
  门槛不同：**FSR 只需 Vulkan 上屏，XEngine 还要设备的 `XEG_spatial_upscale`**，因此真机 Vulkan 而没有
  XEngine 扩展的设备仍可用 FSR（`VulkanCapabilities::srFsrSupported` / `srXengineSupported`，UI 按
  `superResolutionBackends()` 分别置灰）。
- **FSR 的两个 pass**：EASU 采样会话图（`textureGather`，采样器须 NEAREST + CLAMP）写成输出分辨率的中间图，
  RCAS 再锐化进 `srImage_`。一个 render pass 覆盖两者（目标格式都是 `kPictureFormat`），共用全屏三角形与
  `present_quad.vert`。常量在 shader 内用 `FsrEasuCon`/`FsrRcasCon` 从普通 uniform 算，CPU 只传尺寸与锐度，
  **不需要 float→half 位打包**；RCAS 的锐度是「档数」（0 = 最锐）。头文件由 `fetch-sources.ps1` 固定版本拉取，
  缺树时 CMake 警告并只保留 XEngine。
- **两张图**：输入是**会话分辨率的一张图**——RDP 的桌面图，或 Moonlight 每帧槽一张的 video picture
  （`kPictureFormat` = `B8G8R8A8_UNORM`）；输出是**输出图**（同格式、`USAGE = COLOR_ATTACHMENT | SAMPLED`、
  device-local、输出分辨率，**每个帧槽一份**，避免上一帧的 letterbox 还在采样时被下一帧覆盖）。
  通道序仍由图像格式承担，shader 不做交换。letterbox 采样的是**输出图**，源尺寸取输出分辨率
  （再 fit 到窗口）。FSR 的 EASU 输入/中间图、RCAS 输出与描述符都按帧槽各一份，与 picture 槽位一一对应。
- **屏障**：输出图每帧被整幅重写，所以进 pass 前用 `UNDEFINED → GENERAL`（丢弃旧内容，免去布局追踪），
  出 pass 后用 `COLOR_ATTACHMENT_WRITE → SHADER_READ`；输入图到上采样输入的依赖走同一条
  `TRANSFER_WRITE → SHADER_READ` 屏障。按帧槽分开的 EASU/RCAS 图也各走同一条。
- **生命周期**：上采样对象与输出图按「输入尺寸 + 输出尺寸 + 输入视图（按帧槽）」缓存，尺寸或视图变了才重建；
  重建点放在 **acquire 之前**（`EnsureDesktopImageLocked` 之后），这样重建里的 fence 等待不会与
  `AcquireFrameLocked`（它已 reset 本槽 fence）互锁。桌面图重建会一并拆掉超分辨率资源。
- **失败就降级且不重试**（`srFailed_` 锁存）：设备拒绝该尺寸/格式（或库缺失）时不进 pass，直接按桌面图
  letterbox，会话照常可用。排查看 hilog 的 `xengine: spatial upscale` 与
  `vulkan presenter: super resolution`。
- **输入是「会话分辨率的一张图」，与谁出它无关**：`RecordPresentTailLocked` 吃一个 `PresentInput`
  （图 + 视图 + 尺寸 + **它是被怎么写的**：`producerStage`/`producerAccess`），并**自己发那条
  `→ SHADER_READ` 依赖屏障**，随后做（可选）超分、letterbox、swapchain、时间戳、提交。这样"输入就绪"
  这件事归共享层所有，**生产者不可能忘**：
  - RDP 喂桌面图（gdi 合成 → `CmdCopyBufferToImage`，`producer = TRANSFER / TRANSFER_WRITE`）；
  - Moonlight 喂 `PresentVideo` 的会话分辨率 RGB 图（convert render pass，`producer =
    COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE`）。
  因此 Moonlight 不需要跟 RDP 的脏区/合并上屏（carry）机制发生关系；超分对象只按「输入图 + 尺寸」
  缓存与重建，反向引用 RDP 专有成员（如桌面图视图）是**透抽象**，不允许。FSR 的两份 EASU 输入描述符
  按帧槽持有；XEngine 每帧直接收当前槽的输入视图。
- **视频输入（Moonlight 超分专用，`PresentVideo`）**：会话把一个**图形分配器**的 `OH_NativeBuffer` 交来（buffer
  模式下由 `OH_AVBuffer_GetNativeBuffer` 取得，见 [`moonlight-video.md`](moonlight-video.md) §1），
  `VkVideoImport`（`core/hmrdp_vk_video.h`）把它**整块按 R8 字节图导入**（外部内存 + 专用分配 + `TILING_LINEAR`）——
  **R8 + `TILING_LINEAR` 只是给这块内存起一个"字节视图"的名字**：线性图像的定义就是"第 `(x,y)` 个 texel 落在
  第 `y` 行第 `x` 个字节"，而 `VK_OHOS_external_memory` 的导入契约只约束**内存**（`allocationSize` /
  `memoryTypeBits` / 偏移），不约束**语义格式**，所以这一步在规范内。
  再由 convert pass **自己做 YUV→RGB**：片元 `texelFetch` 取 Y 与交错 Cb/Cr，系数（3 行 `cY/cCb/cCr/offset`）
  由 CPU 按**缓冲自己的色彩空间**算好、随该帧槽的 uniform buffer 传入；结果写成**会话分辨率的 picture**，
  之后与桌面图同路。**不做 CPU 回读/整帧拷贝**；视频没有脏区，恒整帧一次。
  - **几何取解码器报的账，不推**：420 半平面的**色度面字节偏移 = `stride` × `sliceHeight`**——`stride` 是字节行距
    （`OH_MD_KEY_VIDEO_STRIDE`），`sliceHeight` 是**单个面在内存里实际分配的行数**（`OH_MD_KEY_VIDEO_SLICE_HEIGHT`，
    ≥ 可见高）。两者连同可见尺寸与裁剪矩形一起由编解码器报出，取处是 `onStreamChanged` 的 `OH_AVFormat` 或
    `OH_VideoDecoder_GetOutputDescription`；平台文档明确要求取用、**不要假定固定值**。字节图的 extent 因此是
    `stride × (sliceHeight + sliceHeight/2)`，采样区是亮度面左上的 `visibleWidth×visibleHeight`（有裁剪矩形时
    再加裁剪原点），片元按**字节坐标**直接取（Y 在 `(x,y)`，Cb/Cr 在 `(x&~1, chromaRow + y/2)` 与其 `+1`；
    两个字节的先后由 `OH_MD_KEY_PIXEL_FORMAT` 给出：NV12 = Cb 在前，NV21 = Cr 在前，未知/未报按 NV12
    并打一次性警告；其它像素格式直接拒绝）。**只有编解码器什么都没报时才回落到分配器的
    `OH_NativeBuffer_GetConfig()`**——那时的行数对齐单位是推的，必须打一次性警告。
    **判据**：可见区之外不可采样——按可见尺寸声明图像会让色度起点落进亮度面的填充区（填充通常为零 ⇒ 色度读到零）。
  - **转换在自己这边，不赌驱动对缓冲的解释**：驱动对这块缓冲的格式描述决定采样拿到什么，本仓不依赖它；**渲染依据
    是缓冲自己的声明**（`OH_NativeBuffer_GetColorSpace`）；缓冲没报色彩空间时取**会话协商的 CSC**
    （`encoderCscMode`，SDR 即 BT.709 limited），不用驱动的 `suggestedYcbcrModel/Range` 兜底（那是 RGB/full 时
    会把 YUV 算错）。BT.601/709 与 narrow/full 都在我们手里。**判据**：分不清"色度读到零"与"采样到原始分量"时
    **不要用画面颜色判**——两者外观相近；要看几何与色彩空间的读数（`video decoder: stream …` 与
    `vulkan video import: …` 两行，后者已带 `cs=` 与 `expected=` 两个读数）。
  - **协商值只作核对基准，不当作渲染依据，也纠正不了平台**：解码侧**没有**"声明输出色彩空间"的接口——
    `OH_MD_KEY_VIDEO_DECODER_OUTPUT_COLOR_SPACE` 只服务 HDR Vivid→SDR 转码（仅 Surface 模式、输入须为
    HDR Vivid 码流，非 HDR Vivid 输入会以 `OH_AVCodecOnError` 报
    `AV_ERR_VIDEO_UNSUPPORTED_COLOR_SPACE_CONVERSION`），不是这里的开关；H.273 那几个键
    （`OH_MD_KEY_MATRIX_COEFFICIENTS`/`RANGE_FLAG` 等）也没有"解码输出可读"的文档依据。所以会话把协商值
    （`SetVideoColorSpace`）只当期望值：缓冲声明与它**不同族或不同范围**（或声明的是本仓根本没有的族）时报
    `color-mismatch`（会话标题 token + 一次提示），渲染仍按缓冲声明。**不静默给错误画面，也不假装能纠正它**。
  - **每帧参数走 uniform buffer，不用 push constant**：片元阶段拿到的 push constant 不可靠（本平台驱动行为），
    而 uniform buffer 与 presenter 的 FSR pass 走的是同一条已验证的路。**规则**：图形管线里需要传给片元的
    逐帧数据一律用 UBO/描述符，不要靠 push constant。
  - **导入配方**（照官方《Vulkan External Memory 开发指导》）：图像声明为**外部内存**
    `VkExternalMemoryImageCreateInfo{ handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS }`
    （API ≥ 23 用 0x8000，`..._OHOS_NATIVE_BUFFER_BIT_OHOS`（0x2000）已废弃）；内存**专用分配**
    `VkMemoryDedicatedAllocateInfo{ image, pNext = &VkImportNativeBufferInfoOHOS }` 挂在 `VkMemoryAllocateInfo.pNext`。
    因为用的是**普通格式**（R8），**不**挂 `VkExternalFormatOHOS`（那是外部格式才需要的）。
    设备需 `VK_OHOS_external_memory` **且**支持 `R8_UNORM + TILING_LINEAR + SAMPLED` 的线性图像
    （`VulkanCapabilities::srVideoImportSupported`，见 §5），不满足就只关 Moonlight 超分。
  - **导入按缓冲缓存，每帧零分配**：解码器的输出缓冲是**会话内固定的小池子**，所以每个缓冲的导入（图像 + 内存 +
    视图 + 每帧槽一份描述符集）只做一次，之后每帧只查表；描述符集随导入建好，`RecordConvert` 只绑不写。
    Cache 有上界；满了只淘汰**最久未用**的那一项，并在淘汰前等 GPU 腾空（`needsDrainFor()` →
    `DrainInFlightLocked`），其余导入保留。分辨率变化连同 picture 一起重建。**判据**：缓冲身份在会话内稳定
    （`OH_NativeBuffer_GetSeqNum`），"每帧 create/destroy 图像"没有理由存在。导入失败按尺寸**锁存**
    （同一尺寸不逐帧重试，避免每帧分配/失败抖动），重建 picture 或新会话才重新尝试。
  - **归还与同步**：buffer 模式的归还接口**不带 fence**，所以"GPU 读完了"只能由我们自己保证——`PresentVideo`
    提交成功时返回**呈现槽位 token**，交接线程保留该帧的 codec 缓冲，下一次轮到同一槽位前调用
    `WaitVideoFrame(token)`，等 fence 后才把缓冲交回编解码器。因此交接线程最多压住**两帧**（两个 picture/UBO
    槽），解码与上一帧的上采样可以重叠；`PresentVideo` 自己也会在写某槽的 picture/UBO 之前等该槽上一笔提交
    （覆盖"上一条没拿到 token"的路径）。这些等待是 GPU 工作，**不计入** §2 的 `presentWait`（那一项只统计
    等显示端的 acquire）；GPU 侧仍由时间戳三段单独计量。
  - **不套用任何朝向/变换矩阵**：解码输出的内容本身已是显示顺序（直出链路把同一编码器的输出直接上屏且方向正确，
    即为证据），所以顶向下采样不需要变换。若将来遇到真正带旋转的生产端，入口是生产端自己的变换
    （`OH_MD_KEY_VIDEO_TRANSFORM_TYPE` 一类的媒体侧声明），而不是把 GL 空间的矩阵作用到顶点位置。

## 5. 呈现与超分能力判定

与设备相关的能力统一按 **Capability 模式**表达（原生优雅降级 + ArkTS 侧 `Capability{supported, reason}` +
UI 置灰），约定见 [`architecture.md`](architecture.md) §5。上屏这一段由一个**呈现判定**、两个**超分后端判定**
与一个**视频导入判定**组成（`FillPresenterVerdict()`，`hmrdp_vk_context.*`）：

| 判定 | 用途 | 条件 |
|---|---|---|
| **呈现判定**（`presenterSupported`） | 上屏后端自动选择（§1） | device + host-visible 内存 + `VK_OHOS_surface`/`VK_KHR_swapchain`；**不需要 compute** |
| **超分：FSR**（`srFsrSupported`） | 超分后端之一（§4） | 呈现判定成立 + 构建里编进了 FSR shader（`HMRDP_HAVE_FSR`） |
| **超分：XEngine**（`srXengineSupported`） | 超分后端之一（§4） | 呈现判定成立 + libxengine 可加载 + 设备报 `XEG_spatial_upscale` |
| **视频导入**（`srVideoImportSupported`） | Moonlight 超分的前置（§4） | 呈现判定成立 + `VK_OHOS_external_memory` + `R8_UNORM/TILING_LINEAR/SAMPLED` 可创建（缓冲按 R8 字节图导入，转换自己做，不依赖驱动解释缓冲） |

- 呈现能力判定**只有 `presenterSupported` 一个**：不存在分开的"解码能力 / 呈现能力"两套判定（解码始终是
  FreeRDP gdi，见 [`rdp-gfx.md`](rdp-gfx.md)）。
- 两个超分后端共用「呈现判定」这一前提（上采样渲染进的都是呈现器的图），但门槛不同：**FSR 只多要构建里
  有它的 shader，XEngine 还要设备特性**。所以 `srSupported` 是「至少一个后端可用」，真机 Vulkan 而无
  XEngine 扩展的设备仍然能用 FSR。
- 原因码（`srUnsupportedCode`）在**一个都没有**时才给出：**先取呈现判定的码**，再是 `no-xengine`
  （libxengine 不可用）与 `no-extension`（设备无空域上采样特性）。`superResolutionSupport()` 回它，
  中文文案在 `DeviceCapabilities.superResolution()`；`superResolutionBackends()` 另回**可用的后端列表**
  （`"xengine"`/`"fsr"`/`"xengine,fsr"`/空）**与 `videoImport` 布尔**，UI 按它逐个置灰。
  `SettingsStore.srBackendUsable(backend, protocol)` 是某个后端能否生效的唯一判据。
- **视频导入**（`srVideoImportSupported`）不是 UI 上的独立开关：它是 **Moonlight 超分的额外前置**
  （`DeviceCapabilities.srVideoImport()`），RDP 超分不需要它（RDP 的输入是 CPU 桌面图）。设备缺任一前提时
  Moonlight 分栏的超分选项置灰，RDP 分栏不受影响。
- **libxengine 按需 `dlopen`，不链接**（同 libvulkan 的理由：缺库要降级，不是加载失败）。
  XEngine（平台文档称「超分」）头文件在 DevEco 的 **HMS sysroot**
  （`<sdk>/default/hms/native/sysroot/usr/include/xengine`），与 toolchain 指向的 openharmony sysroot 同级；
  CMake 按 `${OHOS_SDK_NATIVE}/../../hms/native/sysroot` 定位，找不到就不定义 `HMRDP_HAVE_XENGINE`，
  桥退化为"不支持"而构建照常（见 `hmrdp_xeg.cpp`）。
- **呈现判定实际对应的 Vulkan 面**（`VkRenderer` 用它，逐项可核）：instance 扩展
  `VK_KHR_surface` + `VK_OHOS_surface`；
  device 扩展为 `VK_KHR_swapchain` **加上按需使能的** `VK_OHOS_external_memory` / `VK_KHR_external_memory`
  （只为 Moonlight 的视频导入；缺它只是该路径不可用，不是设备不可用）；一个 graphics+present
  队列族；swapchain（格式优先 `B8G8R8A8_UNORM`，回退 `R8G8B8A8_UNORM`，FIFO）+ render pass/framebuffer/view；
  一条 graphics pipeline（`present_quad.vert/frag`，动态 viewport/scissor 做 letterbox，sampler + descriptor set）；
  持久桌面 image（`B8G8R8A8_UNORM`）与 host-visible staging/零拷贝缓冲；command pool/buffer×2、二进制信号量、
  fence×2；`vkCmdCopyBufferToImage` + `vkCmdPipelineBarrier` + `vkCmdBeginRenderPass/Draw/EndRenderPass` +
  `QueueSubmit/Present`。**用不到**：compute（`CmdDispatch`/`CreateComputePipelines`）、`vkCmdBlitImage`、
  `vkCmdClearColorImage`/`CmdCopyImage`/`CmdFillBuffer`/`CmdCopyBuffer`、timeline 信号量。视频导入路径另用
  `vkGetNativeBufferPropertiesOHOS`、`vkGetPhysicalDeviceImageFormatProperties`（R8 线性探针）与一次全屏
  convert pass（R8 字节图 + `texelFetch` 自带 YUV→RGB，`video_convert.vert/frag`），见 §4。
- **排除模拟器包**（x86_64 构建）：模拟器会按标准接口谎报能力，能力探测排除不掉它；这与"Vulkan 只在真机验证"
  的口径一致，所以模拟器上呈现回落到 GLES。
- `native_window` 是 `libhmrdp` 显式链接的显示栈 API（把 XComponent 的 surfaceId 变成 `OHNativeWindow`，
  交给 Vulkan/EGL）；`EGL`/`GLESv3` 只服务 GLES 兜底呈现器。都是设备必备库，不需要像 OHAudio 那样做缺库降级。

## 6. 账目与探针

- **`present=`**：绝大部分是**固定的 Vulkan 调用**（acquire / 录制 / submit / present），不是能删的活；
  `flush` 只有 µs 级。`presentWait`（阻塞）单列，见 §2。RDP 侧在此基础上多一项 `sync`（零拷贝单缓冲的等待）
  与 `uploaded/box/rectlist/truncated`（脏区上传量），见 [`rdp-present.md`](rdp-present.md) §4。
- **GPU 侧另算**：时间戳查询（每槽 4 个）把一次提交切成 **`copy` / `sr`（超分辨率上采样 pass）/
  `blit`（clear 整张 + letterbox quad）**三段；超分辨率关闭时 `sr` 落在屏障之后、自然接近 0。
  `copy` 在 RDP 是脏区字节 buffer→image，在 Moonlight 视频路径是 **YUV→RGB 的 convert pass**。
  三个数由两个会话各自取走（RDP 进 `gpuCopyUs`/`gpuSrUs`/`gpuBlitUs`，见
  [`rdp-session.md`](rdp-session.md) §5；Moonlight 进 `MoonlightTelemetry`，见
  [`moonlight-session.md`](moonlight-session.md) §2），另有一条 30 帧一打的 hilog。
  **它们只是"本机这一帧让 GPU 干了多久"**：GPU 整机占用率没有对三方应用开放的接口。
  **`blit` 与脏区无关**：swapchain 图像是轮转的（内容在两次呈现之间未定义）⇒ 每帧整幅是默认正确做法；
  要省它只能按 **swapchain 图像**各维护"已写入的增量"。没有 damage-rect 接口可用
  （`vkQueuePresentKHR` 不给，`VK_KHR_incremental_present` 不在设备能力表里且只是提示）；
  **不要**改用 `OH_NativeWindow_*` / `FlushBuffer(..., Region)`。
  - **多大**：GPU 时间戳在整屏帧上是 ~1ms 量级、**与脏区无关**（碎片样本同样）；`copy`（脏区字节）另算。
    ⚠ 由 `sync` 墙钟反推的"GPU 十毫秒级"**不成立**：那是排队，不是 GPU 工作。
  - **为什么不做**：这是每帧整幅的固定成本（~1ms 量级），被 CPU 侧更大的工作量藏住；
    实现它要按 **swapchain 图像**维护已写入增量，收益只是省一次 letterbox 采样。判据是"降成本、fps 够用"。
