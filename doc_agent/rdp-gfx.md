# GFX 码流 / CPU（gdi）链路

本文件是**改 GFX 相关代码前必读**的口径文档。范围：GFX 通道的压缩与命令模型、会话侧的接线约束、
dev 回放与参考画面（仅 RDP 解码/合成调试）、以及 CPU（gdi）链路的成本结构、量测纪律与并行形态（§7）。
上屏这一段见 [`present.md`](present.md)（共享管线）与 [`rdp-present.md`](rdp-present.md)（RDP 侧接线）。

> **不采用**：自研的 GPU 解码/合成引擎（含 compute 解码、`rfx_*.comp`、Progressive 容器解析器与其
> 能力判定）和 H.264/AVC 支持都不要复活（见 §3）。解码/合成只有 FreeRDP gdi 一条路；上屏保留
> Vulkan / GLES 两套呈现器（§2）。

## 0. 管线框架

### 0.1 通道与两层压缩

- 一条 RDP 连接复用多条通道（GFX、输入、剪贴板、音频…），**视觉管线只属于 GFX 通道**
  （动态虚拟通道 `Microsoft::Windows::RDS::Graphics`）；上行（键鼠输入、能力协商、帧回执、缓存导入）
  **不产生画面**。
- GFX 通道里有**两层压缩**：
  1. **传输级 ZGFX**：整条字节流批量压缩，分段且**可原样透传**（压不动就发原样段）。对**已熵编码的图像
     载荷**（Progressive / ClearCodec…）基本压不动（客户端解压≈拷贝），只有未压缩位图/元数据才真正被压缩；
  2. **图像码流**：命令载荷自身的编码（Progressive / ClearCodec / …），这才是真正的"图像解压"。
- 客户端接收链：**ZGFX 解压 → RDPGFX PDU 解析** → 「操作命令 + **仍压缩的**图像载荷」。
- **分工**：ZGFX + PDU 解析留在 CPU（廉价，且产物是必须串行处理的"控制+图像"混合流）；
  图像载荷交给 FreeRDP 的解码器（live 与 CPU 回放同一条 gdi 路径）。

### 0.2 从命令到表面像素

- 客户端为每个 surface 持有**持久缓冲**；服务端用 `surfaceId` 寻址，只下发"对该表面某区域的操作"。
- 图像命令 = 一条码流载荷 + 一个**目标区域**（矩形；渐进式是"分块 + 有效区域"）。
- **顺序是语义的一部分**：严格按命令到达顺序执行；重叠时"后覆盖先"；表面拷贝、缓存存取、渐进细化都
  依赖**当前表面内容**。
- **一致性前提**：客户端表面内容必须与服务端认为的一致——后续命令会读它，且解错**不会自动纠正**
  （除非该区域之后被重新覆盖）。所以客户端解码器与服务端的语义必须严格对齐（用 FreeRDP 自带解码器即可）。
- 像素操作只有四类：**解码写入 / 纯色填充 / 表面拷贝 / 缓存存取**；最终都是"把某矩形内的像素写成
  确定值"，写进**同一张表面图**，多张表面再合成到**可见桌面图**。

### 0.3 各码流（本工程范围）

| codecId | 名称 | 写出方式 | 读旧像素 | 跨命令状态 |
|---|---|---|---|---|
| 0x0000 | Uncompressed | 目标矩形整块覆写 | 否 | 无 |
| 0x0008 | **ClearCodec** | 目标矩形，**可能只覆盖部分像素** | **是** | **会话级**（序列号 + 字形/竖条缓存） |
| 0x0009 | CAPROGRESSIVE | 64×64 分块 + 有效区域、多次细化 | 区域外保留 | **每块持久状态**（本工程主用） |
| 0x000A | Planar | — | — | 检测到即回退 |
| 0x000D | CAPROGRESSIVE_V2 | — | — | 检测到即不支持 |

- **NSC 不是独立 GFX 码流**：它是 ClearCodec 内部的一种子编码，别与 Planar 混淆。
- **渐进分块的顺序**：同一条消息内的分块**互不重叠、可任意顺序**；**跨消息的同一分块必须按序**
  （先粗后细在持久状态上累积）；与其他命令之间仍是全局按序。
- 上述码流全部由 FreeRDP 自带实现处理（`WITH_SIMD` 等构建开关与补丁决定其内部实现，见
  [`rdp-native.md`](rdp-native.md)）。本工程不自研解码/合成。

### 0.4 从表面到屏幕（合成 / 脏区 / 上屏）

1. 只取**映射到输出**的表面（未映射表面不参与上屏）；
2. 取该表面的**脏区**（命令执行时标记的矩形并集）∩ 映射矩形；
3. **合成到主画面**：**1:1 拷贝** + 格式转换 + 裁剪到桌面边界；多表面按顺序叠加（协议未定义输出表面间的
   z 序，实践上不重叠）。服务器端缩放映射不支持；
4. 把写入的矩形并入**主画面脏区**；清空该表面脏区；
5. **上屏门控**：主画面脏区**为空 → 不上屏**（静止，或只收到离屏/缓存类更新）→ **FPS = 0**；
   非空 → **只上传脏矩形**；
6. 服务端等**帧回执**推进下一帧。回执在**解码之后、`EndFrame` 回调之前**写出
   （`native/scripts/patch-steps/09-tcp-frameloop-qos.ps1`）⇒ 本地合成/上屏不占服务端每帧往返。
   **回执节奏即服务端出帧的时钟**：客户端逐帧推迟回执只压住传输、压不住服务端产帧，未发出的画面变化在
   服务端侧累积，**输入回显随之推迟**。

真正的"后处理"只有：**缩放（可选）+ 格式转换 + 裁剪 + 脏区合并**；没有滤波/锐化/去块，也没有跨表面
alpha 混合。远端光标独立处理，不混进主画面缓冲。

### 0.5 "静止 / 部分变动 / 大量变动"

这不是客户端的处理分支，而是**输入形态**（由**服务端**在发送前选择命令/码流决定）：静止=不发绘制命令
（最多管理/回执）→ 无脏区 → 不上屏；部分变动=少量命令/小矩形/缓存拷贝 → 小脏区；大量变动=多条命令/
整屏块 → 大脏区。客户端走**同一条流水线**，显式分支只有：① 命令分类；② 帧内/帧外；③ 是否映射到输出；
④ 主画面脏区是否为空。

## 1. 会话侧约束

- FreeRDP 的 RDPGFX 回调必须接上 `freerdp_client_OnChannelConnectedEventHandler`（绑到
  `ChannelConnected`/`ChannelDisconnected`），否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
- 画面由 gdi 出、经呈现器上屏；遥测四相与账目口径见本文件 §7.1。
- 采集点在 `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册（见
  [`rdp-native.md`](rdp-native.md) §3）。
- 帧流水线（解压/解析/解码/合成/上屏/回执）跑在**会话自己的 GFX 工作线程**上（理由与线程模型见
  [`architecture.md`](architecture.md) §4）。接线（sink 钩子、`HmrdpGfxReplayRecv`、QoS）见
  [`rdp-native.md`](rdp-native.md) §3。

## 2. 上屏（present）

桌面帧的上屏走 [`present.md`](present.md)（一套实现、Vulkan/GLES 两个后端，后端按能力自动选；
判据见 §4），RDP 侧的脏区上传与零拷贝接线见 [`rdp-present.md`](rdp-present.md)。本文件只涉及帧的**产出与合成**。

## 3. 不采用的方向（**不要在新代码里重新引入**）

- **自研 GPU 解码/合成引擎**（Vulkan desktop engine、Progressive/ClearCodec compute 解码、表面合成、
  `rfx_*.comp`、Progressive 容器解析器、引擎能力判定 / `engineSupported`、dev 回放的引擎路线）**不采用**：
  不要引入 compute 解码引擎或 GLES/EGL 引擎后端。
- **H.264/AVC 支持不采用**：它在真机上只有客户端广告 AVC444 时才被服务端启用（属微软非核心可选项），
  且命中硬解也无明显收益 ⇒ 服务端改用其他码流。**不要引入媒体库依赖**。

解码始终是 FreeRDP 的 gdi 软解（上屏后端的选择见 [`present.md`](present.md) §1）。

## 4. 呈现能力判定

见 [`present.md`](present.md) §5（单一 `presenterSupported` 判定、Vulkan 面、模拟器回落）与
§1（上屏后端的选择）。

## 5. 量测纪律（回放）

- **先看计数，再加日志**：回放 stats 的计数（解析错误、`skipped`、`truncated` 等）就是"协议级行为有没有
  按预期发生"的账本。**不要为了查一次问题就新加一次性日志探针**：周期性统计行会很快冲掉缓冲区
  （见 [`testing.md`](testing.md)）。
- **每轮的 `state=` / `run=`**：stats 文本第一行是 `state=running|finished|aborted  run=<n>`，
  多轮测量按它等待与归属，别按内容猜；驱动脚本见 `native/scripts/replay-rounds.ps1`。
- **回放页的「并发 / 路线 / 参考 / 节拍 / 重放」内部都是 `stop + start`**（`并发` 会按新的实例
  数目重建全部实例），在一轮没跑完时点击等于把那一轮掐断；性能数字只取 `(running=0)` 的**整轮**，
  并记下 `frames=` 确认整份跑完（不要假定固定帧数）。判定"跑完"要**轮询** stats 文本里的
  `(running=0)`，不要固定 sleep。
- **两种回放节拍**（dev 页切换，`mode=` 字段）：
  - **`mode=fast`（默认「跑满」）**：完全不打节拍——上一帧做完就喂下一条，**`fps` 就是吞吐上限**，
    不是 live 的出帧率。
  - **`mode=realtime`**：按**录制时记录的到达时刻**喂数据，复现 live 的**帧间隔**（帧间隔本身是负载的
    一部分：CPU 频率/大小核落点、缓存局部性、线程池唤醒代价）。此时 `fps` ≈ 当时的出帧率，可直接与
    live 工具栏读数对照；`lag=` = 落后录制日程的最大值（= 扛不住的量），**不做追赶**。
  - 无时间戳的旧录像（v0，见下）请求 realtime 时回落 `fast`，`mode=` 显示成 **`fast(untimed)`**：
    与普通 `fast` 跑法相同，只是标出它是"请求了 realtime 但录像没时刻"，两次不同 `mode` 的轮次不可比。
  - **realtime 仍不是闭环**：live 的节奏由服务端与每帧帧回执共同决定（回执在该帧解码后、上屏前就发出，
    见 §0.4），回放既不发回执也没有服务端调度。它复现的是"服务器已经产出的那份流的到达节奏"，不是
    "服务器面对一个更快的客户端会怎么发"。**它的 `fps` 是录制节拍本身，不能当"客户端能力"读**。
- **不要用"跳过某一步 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
  - **dev 页驱动**：按控件 `bounds` 点击、窗口置前等规则见 [`testing.md`](testing.md)。
    回放页的按钮在顶栏，状态行会重复同名文字（如 `路线:CPU`），按 `text` 找控件时取顶栏那个。

## 6. 回放与参考画面（RDP 解码/合成调试）

```
设置页「抓取 GFX 码流（测试）」  →  原始码流文件（u32 长度 + payload，见下）
dev 页「回放测试」：并发:N   路线:CPU / 硬件加速   参考:关 / 导出 / 对比
```

这套回放只用于 **RDP gdi 解码/合成**的开发调试与回归：比对的是 gdi 主缓冲的像素，不是产品验收路径，
也不覆盖呈现器或超分。**超分没有对应的回放/参考，不要为它复制这套工具。**

- **多实例（并发）**：一页可同时跑 N 个回放实例，每个实例一个 surface、一条独立管线（自有
  presenter/device、work meter、解码执行器与线程），互不共享可变状态（见
  [`architecture.md`](architecture.md) §4.1）。这是**多会话回归的载体**：不需要 RDP 服务端，
  在模拟器上即可并发跑同一份录像。机读接口是 NAPI 的 `startReplay / stopReplay / resizeReplay /
  replayStats / stopAllReplays / listReplays`（`listReplays` 每实例一行），可完全不经 UI 驱动。
  **判据**：各实例能独立起停、各自 `bad=0`、`frames/presents` 互不串扰（串扰表现为某实例的计数
  或哈希跟着另一个变），且反复起停后每个实例都能真的停下（无卡死、无线程泄漏）。

- **路线只有两条，且是同一条 gdi 解码路径的两种上屏后端**：`CPU` = GLES 上屏（完全不碰 Vulkan）、
  `硬件加速` = Vulkan 上屏。这是 **dev 页专用的临时 A/B**（不落库）：live 会话没有这个开关，
  呈现器按能力自动选（见 [`present.md`](present.md) §1）。
- **比对口径 = 参考画面（golden reference）**：同一条路线先 `参考:导出` 记一遍
  （`hmrdp_ref_<captureTag>.{hash,bmp}`，`captureTag` 是录制内容指纹，换录像即换参考），之后每轮
  `参考:对比` 看 `bad=`。判据、量级读法与"参考模式不是性能模式"见 §7.2。
- **两份基线录像，改完解码/补丁都要跑**：一份**浏览/滚动**（消息稀疏、脏区小）作回归门，
  一份**看视频**（整帧大块变化、每帧多条 Progressive 消息）覆盖另一场景。样本与参考文件都在本地
  gitignore 目录，设备侧用同一个固定文件名喂给回放（`hdc` 只能**覆盖**已存在的文件，不能在该目录新建）。
  **每份新捕获先自己 `参考:导出` + `参考:对比` 过一遍才能当基线**（同名文件的不同录制不能互相背书——
  参考文件名里的指纹就是为此）。
- **呈现方式**：gdi 帧走呈现器接口（`硬件加速` 路线 = Vulkan、`CPU` 路线 = GLES）——与 live 会话是同一条路径，
  所以两者不会各自分叉。**参考判定与呈现路径解耦**：参考哈希读的是 gdi 主缓冲，`present` 只碰 swapchain，
  所以换呈现后端、改重建策略都不影响 `bad`；反过来说 `bad` 变化只能来自解码/合成。
  **任何一轮性能结论的前提是那一轮 `bad=0`**。
- **分叉怎么定位**：先看整屏比对（`bad/rgbPx/bbox/maxDelta`）。**判定分叉只以 gdi 自己的表面/主缓冲
  为准**（用手写镜像代替它会误报）。**不要用"脏区/覆盖集合"比对来判定或定位分叉**：参考实现（gdi）的
  脏矩形是 region16 合并出来的**粗超集**——同一帧可能只有一条跨越整个屏幕的 rect——所以"参考覆盖了、
  这边没覆盖"在**像素完全一致**的帧上也会大量出现。判定/定位只能靠**像素值**。
- **采集内容与格式**：单文件存的是**服务端在 GFX 通道上、ZGFX 之前**的原始字节，采集点在
  `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册（见 [`rdp-native.md`](rdp-native.md) §3）。
  两种布局：
  - **v1（当前）**：文件头 = 8 字节 magic，之后每条 = `u32 长度` + `u64 到达时刻(µs)` + 原始字节。
    到达时刻由 app 侧在回调里取（宿主单调时钟），**不需要改 FreeRDP 补丁**。
  - **v0（旧录像）**：无 magic，每条 = `u32 长度` + 原始字节（无时间信息）⇒ 只能 `fast`。
  只读文件头即可判断布局，所以 dev 页能在打开整份文件前决定 realtime 是否可用。
  录制文件**不入库**（设备端在应用沙箱，本地副本放 gitignore 目录）。
  **已知扰动**：抓取钩子在 RDP 线程上每条 chunk 取锁 + 落盘，而 live 的码流形态是闭环的
  （客户端快慢会影响服务端怎么发），所以录制本身会把被测对象拖慢一点；要求更高的保真度时要把落盘
  挪出 RDP 线程。
- **差分测试（补齐捕获里没有的码流）**：统一方法 = **同一份载荷**分别喂两个实现，逐像素比对。
  载荷优先用 FreeRDP 自带编码器生成；边界要覆盖**尺寸非 64 倍数**、纯色/渐变/UI 文本/alpha。
  ClearCodec 只有解码（运行时必须支持）没有编码器 ⇒ 只能用真实捕获；`CAPROGRESSIVE_V2` 双方都未实现
  ⇒ 对齐"检测到即不支持"。
- 采集与回放需要**打过补丁并重编的 FreeRDP**（见 [`native-build.md`](native-build.md) §2/§3）。

## 7. CPU（gdi）链路：判据、门禁与并行形态

**CPU 路线** = FreeRDP gdi 解码 + 本项目自己的呈现器；这是解码/合成的**唯一路径**（§0）。
§7.1 讲**怎么读这笔账**，§7.2 讲**怎么判它没坏**，§7.3 是这条线**固定的并行形态**。

### 7.1 判据与账目口径

- **判据**：**把每帧的计算成本降下来 = 能耗降下来**；fps 只要"够用"（跟得上服务端的到达节奏、不卡）。
  判收益看每轮 `cpu=`（整轮进程 CPU 时间）与 `本机` 拆相；**不看**单纯墙钟。
  - ⚠ **`cpu=` 只在同一频率档下可比**：动态功耗 ≈ f·V²，多核把 SoC 压到低频档时，同一秒 CPU 时间
    更便宜 ⇒ "并行换了多少 CPU 秒"**不是能量结论**。发回放轮时同轮记下 `run … cpuKHz=`，不同档不横向比。
- **优先"少干活"**（去冗余拷贝、少唤醒、去掉重复搬运），但**不是因为"并行不好"**：多核低频在很多负载下
  比单核高频更省电，**并行本身是正当的能效手段**。
  - ⚠ **并行要收益，调用线程必须进入工作集**（调用方参与，§7.3）：
    同一批 tile 若全部交给 worker 池、调用线程只停在屏障里，解码段墙钟与整轮 `cpu=` 都会明显变差——
    小 region 上甚至**不如串行**。
  - **宽度 = 在线核数，worker 池通常给不出那么多线程**：多出来的 task 只排队，并不换来墙钟。
  - **规模自适应只有一处**：region 太小就降档甚至串行（`HMRDP_MIN_TILES_PER_WORKER`，§7.3）。
    小 region 上"多开线程"的收益低于每 region 的提交/唤醒成本。
- 回放 stats 就是这条线的账：

  | 行 | 含义 |
  |---|---|
  | `perFrame work=… = zgx+parse + decode + compose + present  (+ sync … + presentWait … blocked)` | 每帧的四个工作相位；`work` 是它们在**展示端**相加的结果（计量器只产子项，不存总数），`本机` 用的就是这个和；`sync`/`presentWait` 是阻塞时间（等 GPU 缓冲 / 等显示端），**不计入**（见下） |
  | `run … threads=… cpu=…s  cpuKHz=…` | 该轮的解码宽度、**整轮进程 CPU 时间**与本轮拿到的 SoC 频率档（`cpu=` 只在同一档下可比） |
  | `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp` | **非像素 GFX 命令**；它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 前先看这行** |
  | `gfx setup: <Name> took … us` | 单条结构命令（模式切换/整面清零这类卡顿） |
  | `uploaded/box/rectlist/truncated`、`present=` | 上屏侧：实际交给呈现器的字节、帧时间（细节见 [`present.md`](present.md) §6 与 [`rdp-present.md`](rdp-present.md) §4） |
  | `ref export: …` / `ref compare: refFrames=… checks=… bad=… firstBad=… rgbPx=… maxDelta=… hashMs=…` | **参考画面对比**（golden reference，见 §6/§7.2）：`bad` = 与参考逐帧哈希不同的帧数；`rgbPx/maxDelta/bbox` 只是参考图像那一帧的逐像素差 |

- **`sync` = 等 GPU 放开主缓冲**（`BeginDesktopBufferWrite`；CPU 路线让解码器直接写呈现器缓冲，所以
  **本帧第一次写之前**要等上一帧的 GPU 拷贝读完，等待点见 [`rdp-present.md`](rdp-present.md) §2）。
- **`presentWait` = 等显示端**：`present` 那一段里 `vkAcquireNextImageKHR`/fence（GLES 是
  `eglSwapBuffers`）的阻塞。`present` 只记录制/上传/提交，两者相加才是 present 的整段墙钟。
- 两者都是**阻塞**不是处理 ⇒ 单列、都不进 `本机`；**帧的整段墙钟 = `本机` + `sync` + `presentWait`**
  （回放再加节拍睡眠）。
  - ⚠ **这笔等待天然落在 `zgx+parse` 的窗口里**：帧首钩子（`StartFrame` / 帧内第一条表面命令）在
    `AccountChunkPrefix()` **之前**跑，而抓取的"一条记录"通常就是一整帧的 ZGX 段 ⇒ 该窗口包含这段等待。
    所以测点必须把它交出来（`OnBlockedBeforeFrameWork()`）：不交，`本机` 就把它算两遍、在 GPU 成为慢的
    一侧时虚高到 `sync` 那么多。自检：**`本机 + sync ≈ 1/fps`**——右边明显小于左边就是被算重了。
- **`pace` 是唯一直接剔除的项**：回放的人为节流不是客户端工作。
- **折叠量 vs 真实时间（并行下必须分开读）**：worker 侧求和是**折叠量**（多线程之和，可达墙钟的 ~K 倍），
  与墙钟类量（`本机`/拆相、解码段墙钟）**不能直接比**；串行时二者重合。
- **判读顺序**：① `setup` ② `kB/frame`/`cmds/frame`（内容是否可比）③ `decode` ④ `present` ⑤ `sync`。

### 7.2 RDP 解码/合成的参考比对

- **RDP 解码/合成的回归判据 = 参考画面（golden reference）**，两份基线录像各一套（录制与文件命名见 §6）：
  每轮 `参考:对比` 报 `bad=0`。它**只比对 gdi 主缓冲的像素**：呈现器与超分不在覆盖范围内，
  超分也不适用这套比对。参考文件名带**录制内容指纹**，换一份录像必然缺参考、必须重新导出。
  - ⚠ **它是"输出不变"的门禁，不是"与旧解码器逐位一致"**：解码侧允许**舍入级**改动（`WITH_SIMD` 这类
    SIMD 实现不保证与通用 C 逐位相同），但**必须把差异量出来**：系数/像素最大偏差 **1 量级**、差异像素
    占比**千分之几** = 舍入，可以收；**成百上千** = 实现不同（16 位回绕、饱和当回绕、错索引），必须改回去。
    ⇒ **口径一变就要重录参考**，此后 `bad=0` 只表示"自那次重录起没有再变"（回归门）。
    **解码侧改动要自带量级读数**：当前构建里没有解码侧对拍探针，所以改解码就必须在那一轮用 `rgbPx` +
    `maxDelta` 给出量级，并重录参考。
  - **逐像素诊断**（"差在哪一帧、差多少"）：`rgbPx/alphaPx/maxDelta/bbox` 只覆盖参考图像那一帧；要逐帧
    定位就重新 `参考:导出` 再比。
  - 参考模式**不是性能模式**：每帧对整个桌面做一次哈希（`hashMs=`），这段时间落在 `EndFrame` 里、会算进
    `compose` ⇒ 读性能数字请用 `参考:关` 那一轮。
- **`bad=0` 覆盖不到的顺序约束**：上屏侧的帧序/等待（零拷贝单缓冲）不属于像素内容，
  判定只能靠机制 + 计数器（见 [`rdp-present.md`](rdp-present.md) §2）。

### 7.3 并行形态（固定）

实现位置：app 侧 `entry/src/main/cpp/core/hmrdp_parallel.*`、`hmrdp_decode_tuning.*`；解码侧补丁
`native/scripts/patch-steps/` 11、20、21、22、22b、23、25。补丁对 app 模块的绑定是**弱符号**：
没有这些导出的构建自动退化为串行。宽度、任务划分、执行器都不是运行期开关。

**并行范围与线程归属**

- **唯一并行段 = 一条 Progressive region 的 tile 解码**（`progressive_process_tiles`），外加
  随之搬进该段的 tile 合成拷贝（见下）。并行只发生在一条 region 之内。
- 其余各段都在 **GFX 工作线程**（会话自有；帧流水线已从 drdynvc 分发线程搬来，见
  [`architecture.md`](architecture.md) §4）：

| 段 | 线程 |
|---|---|
| ZGX 解压、PDU 解析、命令分类 | GFX 工作线程 |
| 非像素命令（reset/create/fill/blit/cache…） | GFX 工作线程 |
| region 位流解析（`read`，填 tile 元数据） | GFX 工作线程 |
| **tile 解码 + tile 合成拷贝**（`dec`） | **GFX 工作线程 1 个 + ffrt（宽度 − 1 个）** |
| `update_tiles` 剩余部分（遍历 + 脏区记账） | GFX 工作线程 |
| surface → primary 合成、present 提交 | GFX 工作线程（GPU 侧另算） |

- **消息间不重叠**：一条消息按 `read → dec → update` 接力完成后才处理下一条；没有跨消息流水线。
- **串行分支**（宽度 ≤ 1）：同一 tile 循环直接在 GFX 工作线程执行，不建队列、不提交、不等待；
  工作缓冲用 0 号槽。

**执行器与宽度**

- **执行器 = 平台任务队列（ffrt 并发队列），每会话 / 每回放实例一个**（`DecodeExecutor`，由 app 的
  `hmrdp_parallel.*` 提供），解码器经弱符号使用：`HmrdpDecodeWidth()` 决定串行/并行分支；
  `HmrdpParallelAvailable()` / `HmrdpParallelRun(tasks, fn, ctx)` 提交与等待。符号不解析 ⇒
  宽度读作 1 ⇒ 串行。
- **执行器归属**：`FramePipeline` 持有，一个桌面一份，绝不进程共享——两个会话同时解码不会互相拆
  队列。解码入口（`HmrdpGfxReplayRecv`）以 `DecodeExecutorScope`（线程局部）把"当前执行器"告知
  `HmrdpParallelRun`；该线程未绑定执行器时，整条 region 在调用线程内联执行。
- 队列形态：`ffrt_queue_concurrent`，`max_concurrency` = 当前宽度 − 1；队列与每个任务都带属性
  （name + `ffrt_qos_user_initiated`）；屏障 = 逐任务 handle `ffrt_queue_wait`。宽度变化时销毁重建
  该执行器自己的队列（仅在其所有者无任务在途时发生，因为 `Run` 先等完自己的任务）。
- **调用方参与（caller participation）**：GFX 工作线程自己跑一个 chunk，队列只跑剩下 `宽度 − 1` 个
  （先提交、再跑自己的、最后等屏障）。region 用的线程数仍是宽度个，差别是其中一个从"停在屏障里"
  变成"干活"：没有线程在 tile 还可领取时空转；向 worker 池少要一个线程（自动档宽度 = 在线核数，
  池子可能给不出那么多）；且这一份跑在调用线程自己的上下文里（cache 热、QoS 是它自己的），而不是
  一个刚被唤醒的 worker 上。分块仍按 home + 段尾偷取，所以它做完自己的 home 会继续领块，快的那
  一侧自然多领。
- `HmrdpParallelRun` 的分支：当前线程无绑定执行器 ⇒ 整条 region 内联执行；有执行器时 `tasks <= 1`
  或宽度 ≤ 1 在调用线程内联执行；`tasks > 128` 拒绝；单个提交失败的任务在调用线程补跑，保证一条
  region all-or-nothing。
- **WinPR 池不参与**（补丁 25）：平台执行器可用时 `rfx.c` 置 `UseThreads = FALSE`，不建池。
- **QoS**：tile worker 的 QoS 来自队列/任务属性；GFX 工作线程的 QoS 由 app 注册的钩子
  （`HmrdpSetThreadQoSApplier`，补丁 09）在该线程入口调用一次（drdynvc 线程入口也仍调用它，但那
  现在只剩拆包转发）。
- **宽度 = 在线核数（上限 16）**，没有设置项：`hmrdp::DecodeThreads()` 只数 `sysconf` 的在线核。
  解码器在**每条 region 边界**按需读 `HmrdpDecodeWidth()`，所以进程一起来就是这个宽度，无需
  任何应用。性能核探测（`cpuinfo_max_freq`）只出现在日志行里，不参与宽度决策。

**任务划分（region → 任务 → tile）**

- **任务数按 region 的规模定**：`numChunks = min(宽度, numTiles / HMRDP_MIN_TILES_PER_WORKER)`，
  cap 至 `HMRDP_TILE_CHUNKS = 64`（`hmrdp_region_chunks()`，补丁 21）。一个任务一个 home 区间，
  任务描述符 `PROGRESSIVE_TILE_CHUNK_PARAM` 携带共享参数表、scratch 槽与 home 数组（补丁 20/21）。
  一个线程至少要拿到 `HMRDP_MIN_TILES_PER_WORKER` 个 tile 才值得被唤醒，所以**小 region 自动降档**：
  `numChunks <= 1`（即 `numTiles < 2 × 阈值`）时直接走串行分支，连队列都不提交。
  **这是这条线唯一的规模自适应点**（定档值与判据见下）。
- **划分 = home + 段尾块偷取**（唯一形态，没有对照档）：任务 h 的 home 区间为
  `[h·N/K, (h+1)·N/K)`，K = `numChunks`。任务先解自己的 home——按 `HMRDP_TILE_CLAIM = 2` 个
  tile 的块用 `__sync_fetch_and_add` 领取；home 解完后按 `(homeIndex + round) % homeCount` 轮询
  偷取其他 home 的剩余块（同样按块领取）。**块大小是"尾块不均衡"与"领取流量"的折中**：块越小，
  最后一个块被慢线程攥住的时间越短，但共享游标上的原子操作越多（判据见下）。
  **调用线程也是这些 home 之一的所有者**，所以快的那一侧自然会多领。
- **不变约束**：一个 tile 仍由单次 `progressive_process_tiles_tile_work_callback` 独占解码；
  划分只决定 tile 由哪个任务、按什么顺序处理（同一条 region 的 tile 互不重叠，任意顺序像素等价）。

**内存归属**

- **工作缓冲（scratch，补丁 20）**：per-context 一次性分配 `tileScratch` arena——64 个槽，
  每槽两块缓冲（系数工作缓冲 + 逆 DWT scratch，各 `(8192+32)×3` 字节）。chunk 回调把本任务的
  槽指针写进 `_Thread_local g_HmrdpTlsTileScratch`，tile 解码经 `hmrdp_tile_scratch()` 取用；
  串行分支固定槽 0。tile 解码路径不走 `bufferPool` 的 Take/Return。
- **持久状态（补丁 22）**：每 tile 的三份跨消息常驻缓冲 `sign`/`current`/`data` 改为
  **surface 级连续 arena**（`HMRDP_TILE_ARENA` 编译开关）：tile-major 布局——一个 tile 的
  三份在同一段连续内存、段起点 64 字节对齐；arena 随 tile cache 增长整体重建并复制旧块
  （预测器状态跨 resize 保留）；tile 结构不拥有缓冲。首次绑定时显式清零
  （`sign`/`current` 为 0、`data` 为 0xFF）。
- **目标 surface**：tile 解码直接写入（见下）；多个 worker 写同一 surface 的不同行，互不重叠由
  划分保证（tile 互不重叠 + home 连续）。

**worker 侧合成拷贝与 `update_tiles`**

- **直写（补丁 23，`HMRDP_WORKER_TILE_COPY` 编译开关）**：`progressive_decompress` 把目标缓冲
  （指针/格式/步长/偏移/surface）暂存进 context；region 解码前按 region rects 建合并 clip
  （region16 union，与 `update_tiles` 同法）并计算 clip 哈希 `hmrdp_clip_hash`（折入每条消息
  递增的 `hmrdpMsgSeq`，跨消息不重复）。tile 回调**解码完成一块就**用该 clip 把 tile 直写
  目标 surface（`hmrdp_tile_copy_now`，几何守卫与 `hmrdp_composite_tile` 一致，
  `FREERDP_KEEP_DST_ALPHA`），并给 tile 记下 `hmrdpCopied` + 所用 clip 的哈希。
- **`update_tiles`（GFX 工作线程）**：工作集 = 「裁剪矩形覆盖到的 tile 范围 ∩ 本帧解码过的 tile」
  （补丁 22b：tile 记 `hmrdpFrameId`，`updateStamp` 保证每 pass 每 tile 访问一次）。对每个
  访问到的 tile：`hmrdpCopied` 且 clip 哈希等于本 pass 的 `hmrdpUpdateClip` 时**跳过像素拷贝**
  （只留脏区记账），否则照旧拷贝（后写覆盖）。脏区记账 = 每 tile 行一个 span
  （`hmrdpDirtyLeft/Right/Any`，O(1)；补丁 14）。region 解码结束清空暂存 clip。
- **结果不变性**：同一批 tile、同样的裁剪与像素、同样的脏区；门禁 = 参考画面 `bad=0`（§7.2）。

**编译期开关与定档**

- **没有运行期开关**：宽度、任务划分、执行器、规模阈值都是固定的。要对照就改代码重编，按 §7.1 的
  口径比（`cpu=` 同频档 + `本机` 拆相），**不要**为了 A/B 在树里留第二套路径。
- **编译期开关**：`HMRDP_TILE_ARENA`（补丁 22）、`HMRDP_WORKER_TILE_COPY`（补丁 23）。
- **常量（都按实测定档，改动要重编 + 回归）**：
  - `HMRDP_MIN_TILES_PER_WORKER = 64`：小 region 上开满宽度不划算——每个 chunk 只有十几 tile 时，
    唤醒与领取争用把 queue 段墙钟拉长；降到每 region 实际只开几个线程后 queue 段墙钟明显下降，
    region 大的样本不受影响。判据：**若"宽度开满但线程大半闲着、任务排队延迟占 region 墙钟可观
    比例"，就是开多了，把阈值往上调**。
  - `HMRDP_TILE_CLAIM = 2`：领取/偷取的块大小。块越小，最后一个块被慢线程攥住的时间越短（尾块
    不均衡 ≤ 一个块），但共享游标上的原子操作越多；块再小到 1 则领取流量翻倍，停在 2。
  - `HMRDP_TILE_CHUNKS = 64`：chunk 描述符与 scratch 槽的上界，不是调参项。
