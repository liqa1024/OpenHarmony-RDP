# GFX 码流 / CPU（gdi）链路

本文件是**改 GFX 相关代码前必读**的口径文档。范围：GFX 通道的压缩与命令模型、会话侧的接线约束、
dev 回放与**参考画面**验收、以及 CPU（gdi）链路的成本结构与量测纪律（§8）。
上屏这一段见 [`present-pipeline.md`](present-pipeline.md)；CPU 并行形态见
[`cpu-accel-plan.md`](cpu-accel-plan.md)。

> **已移除**：自研的 Vulkan GFX 桌面引擎（GPU Progressive/ClearCodec 解码 + 表面合成）、它的能力判定、
> dev 回放的引擎路线、`rfx_*` compute 着色器与 Progressive 容器解析器，**已全部从代码删除**。
> 解码与合成现在只有 FreeRDP 的 gdi 一条路；上屏保留 Vulkan / GLES 两套呈现器（§3、§4）。
> 后续若做 GPU 优化，以**现有这套 Vulkan 上屏**为基础重建，不要复活旧的解码/合成引擎。

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
  [`native-libraries.md`](native-libraries.md)）。本工程不再自研解码/合成。

### 0.4 从表面到屏幕（合成 / 脏区 / 上屏）

1. 只取**映射到输出**的表面（未映射表面不参与上屏）；
2. 取该表面的**脏区**（命令执行时标记的矩形并集）∩ 映射矩形；
3. **合成到主画面**：**1:1 拷贝** + 格式转换 + 裁剪到桌面边界；多表面按顺序叠加（协议未定义输出表面间的
   z 序，实践上不重叠）。服务器端缩放映射不支持；
4. 把写入的矩形并入**主画面脏区**；清空该表面脏区；
5. **上屏门控**：主画面脏区**为空 → 不上屏**（静止，或只收到离屏/缓存类更新）→ **FPS = 0**；
   非空 → **只上传脏矩形**；
6. 上屏完成后服务端等**帧回执**（通道层在 `EndFrame` 返回后发送）。

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
- 画面由 gdi 出、经呈现器上屏；会话遥测的四个工作相位见
  [`session-and-input.md`](session-and-input.md) §3。
- 采集点在 `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册（见
  [`native-libraries.md`](native-libraries.md) §3）。

## 2. 上屏（present）

上屏只有一套实现、两个后端：**Vulkan 优先，Vulkan 不能上屏时回落 GLES**。判据是**呈现能力**
（`presenterSupported`：device + host-visible 内存 + surface/swapchain，不需要 compute）。
「硬件加速」开关只选上屏后端（关 = GLES 且不碰 Vulkan）。细节（脏区上传、通道序、零拷贝桌面缓冲、
等待点、swapchain 重建）见 [`present-pipeline.md`](present-pipeline.md)。

## 3. 已移除的方向（**不要在新代码里重新引入**）

- **自研 GPU 解码/合成引擎**（Vulkan desktop engine、Progressive/ClearCodec compute 解码、表面合成、
  `rfx_*.comp`、Progressive 容器解析器、引擎能力判定 / `engineSupported`、dev 回放的引擎路线）：
  已整体删除。**不要再引入 compute 解码引擎或 GLES/EGL 引擎后端**。
- 已移除的 H.264/AVC 支持：它在真机上只有客户端广告 AVC444 时才被服务端启用（属微软非核心可选项），
  且命中硬解也无明显收益 ⇒ 整体砍掉，服务端改用其他码流。**不要再引入媒体库依赖**。

## 4. 呈现与呈现能力判定

- 呈现能力与解码后端**不再有两套判定**：只有 `presenterSupported` 一个。
- 模拟器一律回落 GLES 呈现器（与"Vulkan 只在真机验证"一致）；若以后要让模拟器用 Vulkan 上屏，
  只需改 `FillPresenterVerdict()` 里那一处 ABI 分支。
- Vulkan 面与判定口径见 [`native-libraries.md`](native-libraries.md) §6。

## 5. 量测纪律（回放）

- **先看计数，再加日志**：回放 stats 的计数（解析错误、`skipped`、`truncated` 等）就是"协议级行为有没有
  按预期发生"的账本。**不要为了查一次问题就新加一次性日志探针**：周期性统计行会很快冲掉缓冲区
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）。
- **每轮的 `state=` / `run=`**：stats 文本第一行是 `state=running|finished|aborted  run=<n>`，
  多轮测量按它等待与归属，别按内容猜；驱动脚本见 `native/scripts/replay-rounds.ps1`。
- **回放页的「路线 / 参考 / 节拍 / 重新回放」内部都是 `stop + start`**，在一轮没跑完时点击等于把
  那一轮掐断；性能数字只取 `(running=0)` 的**整轮**，并记下 `frames=` 确认整份跑完（不要假定固定帧数）。
  判定"跑完"要**轮询** stats 文本里的 `(running=0)`，不要固定 sleep。
- **两种回放节拍**（dev 页切换，`mode=` 字段）：
  - **`mode=fast`（默认「跑满」）**：完全不打节拍——上一帧做完就喂下一条，**`fps` 就是吞吐上限**，
    不是 live 的出帧率。
  - **`mode=realtime`**：按**录制时记录的到达时刻**喂数据，复现 live 的**帧间隔**（帧间隔本身是负载的
    一部分：CPU 频率/大小核落点、缓存局部性、线程池唤醒代价）。此时 `fps` ≈ 当时的出帧率，可直接与
    live 工具栏读数对照；`lag=` = 落后录制日程的最大值（= 扛不住的量），**不做追赶**。
    无时间戳的旧录像自动回落 `fast` 并在日志里说明。
  - **realtime 仍不是闭环**：live 的节奏由服务端与每帧帧回执共同决定（回执在我们的上屏之后才发出），
    回放既不发回执也没有服务端调度。它复现的是"服务器已经产出的那份流的到达节奏"，不是"服务器面对一个
    更快的客户端会怎么发"。**它的 `fps` 是录制节拍本身，不能当"客户端能力"读**。
- **不要用"跳过某一步 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
- **dev 页驱动：别用盲点坐标**。先 `dumpLayout` 取控件 `bounds` 再点中心（见
  [`build-and-verify.md`](build-and-verify.md) §5.1）；**按钮在顶栏（y 很小），而状态行会重复同名文字**
  （如 `路线:CPU`），按 `text` 找控件时要取顶栏那个。
  - ⚠ 驱动脚本用坐标点击，所以**窗口必须在最前**：应用被系统重启/窗口被切走时，点击会落到桌面上
    （症状：点了没反应，脚本会报"click did not start a new run"）。脚本因此先 `aa start` 把页面拉起来。

## 6. 验证回路

```
设置页「抓取 RFX 码流（测试）」  →  原始码流文件（u32 长度 + payload，见下）
dev 页「回放测试」：路线:CPU / 硬件加速   参考:关 / 导出 / 对比
```

- **路线只有两条，且是同一条 gdi 解码路径的两种上屏后端**：`CPU` = GLES 上屏（完全不碰 Vulkan）、
  `硬件加速` = Vulkan 上屏（就是设置里的「硬件加速」，页面上的按钮与它是同一个值）。
- **验收口径 = 参考画面（golden reference）**：同一条路线先 `参考:导出` 记一遍
  （`hmrdp_ref_<captureTag>.hash` + `.bmp`，`captureTag` 是录制内容的指纹，**换一份录像就换一套参考**），
  之后每轮 `参考:对比` 报
  `ref compare: refFrames=… checks=… bad=0 firstBad=-1 imageFrame=… rgbPx=0 maxDelta=0`：
  `bad` = 逐帧哈希与参考不同的帧数（目标 **0**），`rgbPx/maxDelta/bbox` 是参考**图像那一帧**的逐像素差
  （用来判断"哪一帧、差多少"，不是全集）。失败时会把那一帧 dump 成 `…_fail.bmp`。
   - ⚠ **参考模式不是性能模式**：它每帧对整个桌面做一次哈希（`ref export/compare` 行报 `hashMs=`），
     这段时间落在 gdi 的 `EndFrame` 里、会算进 `compose` ⇒ 读性能数字时用 `参考:关` 那一轮。
   - ⚠ **参考的口径跟着构建走**：解码侧允许舍入级改动（`WITH_SIMD` 开关、逐位等价之外的 SIMD 改写）
     ⇒ 改完必须**重录**参考，此后 `bad=0` 只表示"自那次重录起没有再变"（回归门）；改动的量级由
     `dwt check: … maxDelta=` 与 `rgbPx/maxDelta` 给（§8.4）。
- **两份基线录像，改完解码/补丁都要跑**：一份**浏览/滚动**（消息稀疏、脏区小）作回归门，
  一份**看视频**（整帧大块变化、每帧多条 Progressive 消息）覆盖另一场景。样本与参考文件都在本地
  gitignore 目录，设备侧用同一个固定文件名喂给回放（`hdc` 只能**覆盖**已存在的文件，不能在该目录新建）。
  **每份新捕获先自己 `参考:导出` + `参考:对比` 过一遍才能当基线**（同名文件的不同录制不能互相背书——
  参考文件名里的指纹就是为此）。
- **呈现方式**：gdi 帧走呈现器接口（`硬件加速` 开 = Vulkan、关 = GLES）——与 live 会话是同一条路径，
  所以两者不会各自分叉。**参考判定与呈现路径解耦**：参考哈希读的是 gdi 主缓冲，`present` 只碰 swapchain，
  所以换呈现后端、改重建策略都不影响 `bad`；反过来说 `bad` 变化只能来自解码/合成。
  **任何一轮性能结论的前提是那一轮 `bad=0`**。
- **分叉怎么定位**：先看整屏比对（`bad/rgbPx/bbox/maxDelta`）。**判定分叉只以 gdi 自己的表面/主缓冲
  为准**（用手写镜像代替它会误报）。**不要用"脏区/覆盖集合"比对来判定或定位分叉**：参考实现（gdi）的
  脏矩形是 region16 合并出来的**粗超集**——同一帧可能只有一条跨越整个屏幕的 rect——所以"参考覆盖了、
  这边没覆盖"在**像素完全一致**的帧上也会大量出现。判定/定位只能靠**像素值**。
- **采集内容与格式**：单文件存的是**服务端在 GFX 通道上、ZGFX 之前**的原始字节，采集点在
  `zgfx_decompress` 之前，由 FreeRDP 补丁以运行期回调注册（见 [`native-libraries.md`](native-libraries.md) §3）。
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
- 采集与回放需要**打过补丁并重编的 FreeRDP**（见 [`native-libraries.md`](native-libraries.md) §2/§3）。

## 7. 待办

- **CPU（gdi）链路的多核并行**（并行范围、ffrt 执行器、宽度、任务划分、内存归属、合成归属）：
  单独成文 → [`cpu-accel-plan.md`](cpu-accel-plan.md)。单核部分的知识见本文件 §8。
- **若重建 GPU 优化**：以现有的 Vulkan 上屏（`VkRenderer` + `hmrdp_vk_context`）为基础；呈现能力判定
  已是唯一判定（§4），不要再引入第二套引擎判定与开关。

## 8. CPU（gdi）链路：成本结构、账目与量测纪律

**CPU 路线** = FreeRDP gdi 解码 + 我们自己的呈现器；这是解码/合成的**唯一路径**（§0）。这一节是这条线的
**通用知识**：成本怎么读、账怎么记、门禁是什么、哪些估算被否证过。这条线现在的并行形态
（并行范围、执行器、宽度、任务划分、内存归属）见 [`cpu-accel-plan.md`](cpu-accel-plan.md)；
历史口径与旧数据见 [`cpu-path_old.md`](cpu-path_old.md)（old）。

### 8.1 判据与账目口径

- **判据**：**把每帧的计算成本降下来 = 能耗降下来**；fps 只要"够用"（跟得上服务端的到达节奏、不卡）。
  判收益看每轮 `cpu=`（进程 CPU 时间）、`本机` 拆相、GPU/DRAM 流量；**不看**单纯墙钟。
  - ⚠ **`cpu=` 只在同一频率档下可比**：动态功耗 ≈ f·V²，多核把 SoC 压到低频档时，同一秒 CPU 时间
    更便宜 ⇒ "并行换了多少 CPU 秒"**不是能量结论**；要么在同一 `cpuKHz=` 档下比，要么按
    "CPU 时间 × 档位"估，要么直接看 duty 下的功耗。
- **优先"少干活"**（去冗余拷贝、少唤醒、去掉重复搬运），但**不是因为"并行不好"**：多核低频在很多负载下
  比单核高频更省电，**并行本身是正当的能效手段**。这条线的并行形态（平台任务队列、宽度、任务划分、
  内存归属）见 [`cpu-accel-plan.md`](cpu-accel-plan.md)。
  - ⚠ **并行要收益，调用线程必须进入工作集**（调用方参与，[`cpu-accel-plan.md`](cpu-accel-plan.md) §1）：
    同一批 tile 若全部交给 worker 池、调用线程只停在屏障里，`dec` 墙钟、`par` 的 `work`/`wall` 与
    整轮 `cpu=` 都会明显变差——小 region 上甚至**不如串行**。判据就是这一组：`work`/`wall` 随
    "那个 chunk 由哪个线程跑"起落，而不是随宽度。
  - **宽度 = 在线核数，worker 池通常给不出那么多线程**：多出来的 task 只排队（`wait/task`
    升、`idle` 升），并不换来墙钟。调用方参与同时把这一份从池子里拿回来。
- 回放 stats 就是这条线的账：

  | 行 | 含义 |
  |---|---|
  | `perFrame work=… = zgx+parse + decode + compose + present  (+ sync … + presentWait … blocked)` | 每帧的四个工作相位；`work` 是它们在**展示端**相加的结果（计量器只产子项，不存总数），`本机` 用的就是这个和；`sync`/`presentWait` 是阻塞时间（等 GPU 缓冲 / 等显示端），**不计入**（见下） |
  | `prog ms/frame: read / dispatch / dec / update  (calls= unions= tiles= tilesDec= ffrt=)` | `decode` 的内部：`read` 读输入位流、`dispatch` 投递 tile（并行才有）、**`dec` = tile 解码段**（并行时是并行段墙钟，串行时是本线程逐 tile；读前先看 `threads=`）、`update` = `update_tiles` 整段（其中像素拷贝默认已随解码段并行，见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §4）；`calls` 消息数、`unions`/`tiles` = `update_tiles` 的并集次数与被访问 tile 数、`tilesDec` = 真正解码的 tile 数、`ffrt` = 走平台队列的 region 次数 |
  | `prog2 ms/frame (sampled 1/16, n=…): rlgr / dequant+diff / idwt / state / upgrade / color  sum=` | **`dec` 之内的拆相**（1/16 采样探针）；`state` = 系数状态拷贝（`sign`/`current`）、`color` = `yCbCrToRGB` + 写 tile。并行下 `sum` 是所有 worker 的时间之和（≈ `dec` × 有效宽度）⇒ **只读占比** |
  | `run … threads=… ffrt=… parRatio=… cpu=…s  cpuKHz=…` | 该轮的解码宽度、平台队列实际派发的 region 次数（证明解码确实走了 ffrt）、**频不变并行效率**（worker 忙碌和 ÷ `dec` 墙钟）与**整轮进程 CPU 时间**；`cpuKHz` 是本轮拿到的 SoC 频率档 |
  | `par busy=…% idle=…% (regions= tasks= Kavg= wall= capacity= work= wait= wait/task=)` | 并行段的 **worker 侧账目**：`capacity = tasks×wall`（`tasks` = 该 region 选用的线程数，由 `gfx` 侧的 tile 数阈值决定，见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §2）是这条 region **要的**线程时间，`work` = 回调执行时间和，`idle = capacity − work`，`Kavg = capacity/wall` = 按墙钟加权的平均实际线程数（**小于 `threads=` 就说明阈值在降档**，它也小于"池子真给了多少"）。`work ≤ capacity` 恒成立（任一时刻在跑的回调 ≤ `tasks`）；`wait` 是任务排队延迟，**单列**（与在跑重叠，可超过 `capacity`）。除 `wall` 外都是**折叠量**。`work ≤ capacity` 是测量自检（`idle` 为负 = 测错）。口径见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §5 |
  | `setup ms/frame: reset/create/delete/map/fill/blit/cache/imp` | **非像素 GFX 命令**；它们本来落在 `zgx+parse` 里 ⇒ **读 `zgx+parse` 前先看这行** |
  | `gfx setup: <Name> took … us` | 单条结构命令（模式切换/整面清零这类卡顿） |
  | `uploaded/box/rectlist/truncated`、`present=` | 上屏侧：实际交给呈现器的字节、帧时间（细节见 [`present-pipeline.md`](present-pipeline.md)） |
  | `ref export: …` / `ref compare: refFrames=… checks=… bad=… firstBad=… rgbPx=… maxDelta=… hashMs=…` | **参考画面对比**（golden reference，见 §6 与 8.4）：`bad` = 与参考逐帧哈希不同的帧数；`rgbPx/maxDelta/bbox` 只是参考图像那一帧的逐像素差 |
  | `dwt check: tiles=… mismatch=… maxDelta=…` | **解码侧改动的对拍**（`参考:对比` 那一轮按 1/16 采样，见 8.4） |

- **`sync` = 等 GPU 放开主缓冲**（`BeginDesktopBufferWrite`；CPU 路线让解码器直接写呈现器缓冲，所以
  **本帧第一次写之前**要等上一帧的 GPU 拷贝读完，等待点见 [`present-pipeline.md`](present-pipeline.md) §4）。
- **`presentWait` = 等显示端**：`present` 那一段里 `vkAcquireNextImageKHR`/fence（GLES 是
  `eglSwapBuffers`）的阻塞。`present` 只记录制/上传/提交，两者相加才是 present 的整段墙钟。
- 两者都是**阻塞**不是处理 ⇒ 单列、都不进 `本机`；**帧的整段墙钟 = `本机` + `sync` + `presentWait`**
  （回放再加节拍睡眠）。
  - ⚠ **这笔等待天然落在 `zgx+parse` 的窗口里**：帧首钩子（`StartFrame` / 帧内第一条表面命令）在
    `AccountChunkPrefix()` **之前**跑，而抓取的"一条记录"通常就是一整帧的 ZGX 段 ⇒ 该窗口覆盖了这次等待。
    所以测点必须把它交出来（`OnBlockedBeforeFrameWork()`）：不交，`本机` 就把它算两遍、在 GPU 成为慢的
    一侧时虚高到 `sync` 那么多。自检：**`本机 + sync ≈ 1/fps`**——右边明显小于左边就是被算重了。
- **`pace` 是唯一直接剔除的项**：回放的人为节流不是客户端工作。
- **折叠量 vs 真实时间（并行下必须分开读）**：并行段里只有墙钟类量是**真实时间**（`本机`/拆相、
  `dec` 的并行口径、`par` 的 `wall`）；worker 侧求和类是**折叠量**——多线程之和，可达墙钟的 ~K 倍
  （`HmrdpProgStat[9]`、`prog2` 的 `sum`、`par` 的 `capacity`/`work`/`idle`/`wait`）。
  **两类不能直接比**；折叠量要跨宽度比，只能除以"提供它的线程时间"（`capacity`）。串行时二者重合。
- **判读顺序**：① `setup` ② `kB/frame`/`cmds/frame`（内容是否可比）③ `dec`（先看 `threads=`）
  ④ `update` ⑤ `present` ⑥ `sync`。

### 8.2 成本结构（量级，不是结论）

两类样本给出这条线的形状（跑满、无节拍）：

| 样本类型 | `本机`/帧 | 拆相形状 |
|---|---|---|
| **整屏变化**（每帧上千 tile，如看视频） | 几十 ms 量级 | **`decode` 占 9 成以上**（内部大半是 `dec` 的 tile 解码，其次 `update`），`zgx+parse`/`present` 各几个百分点 |
| **碎片**（命令多、矩形小但**总量不小**） | 十 ms 量级 | `decode` 约 6 成、`zgx+parse`/`present` 各约 2 成；**`sync` 可达帧墙钟的三分之一** |

- 跨样本比较必须先按**"每帧字节/命令数"归一**。（小矩形 + 小字节的轻样本是纯 CPU 受限；命令多且每帧
  脏区字节也大的样本上，帧率上限由 CPU 与那次脏区拷贝**共同**决定——压 `本机` 只买到能耗。）
- `dec` 之内（`prog2`，1/16 采样）的量级：`idwt` / `state` / `color` / `rlgr` / `upgrade` / `dequant`
  依次递减；**被默认当成"大头"的 RLGR 其实最小**。
  - ⚠ **`idwt` 跑的是"抽取/外推"那一支，不是 `rfx_dwt_2d_decode_block`**：区域带
    `RFX_DWT_REDUCE_EXTRAPOLATE` 时解码器走 `rfx_dwt_2d_extrapolate_decode`（`progressive_rfx_idwt_x/_y`），
    全屏 Progressive 码流的每个区域都是这一支——对拍计数会显示 `codec/rfx_dwt.c` 的通用实现
    `rfx_dwt_2d_decode` **一次都没进**（`tiles=0`）。两支的算术也不同：抽取支用**截断除 2**（`(a+b)/2`）
    而不是算术右移，负数上两者不同，照抄时不能写成 `>>1`。抽取支的形状是
    `X2 = L − (H0+H1)/2`、`X1 = (X0+X2)/2 + 2*H0`、`X0 = X2`：**递推在内层**，且纵向那一趟原来按**列**走
    （每个样本换一条 cache line）——这两条是它比算术量更贵的原因。
  - **`dec` 的两种口径**（同一个"tile 解码段"，测点不同、不可混读）：并行时是**并行段的墙钟**（提交后到
     所有任务等完，`blocked` 是其中真正阻塞的部分）；串行时是**本线程逐 tile 解码的时间**（此时
     `dispatch`/`blocked` 必然为 0，不是缺失）。⇒ 读 `dec` 前先看 `threads=`。
  - **`update` 的大头本来是像素拷贝，不是记账**：`freerdp_image_copy_no_overlap` 把每个 tile（64×64×4B）
     拷进 surface；带 `KEEP_DST_ALPHA` 掩码（每像素一次掩码写）而不是 memcpy。region16 记账只值
      ~0.5ms/帧 量级（见 8.5）。**这份拷贝默认已经随 tile 解码段并行**（`update_tiles` 只留记账，见
      [`cpu-accel-plan.md`](cpu-accel-plan.md) §4），所以并行下 `update` 只剩记账量级。
- **按字节算这笔账**（优化后期，单核的天平已经从"算力"倒向"每帧过多少字节"）：一个 tile（3 分量）大致
  搬 200KB 量级——RLGR 出 `sign`、去量化 `sign → buffer`、状态更新 `buffer↔current`、逆 DWT 三级级联、
  `color` 读系数写 tile、`update` 读 tile + 读目的（保 alpha）+ 写目的。各趟实测速率在
  **L1 级（几十 GB/s）到 DRAM 级（~10GB/s）**之间；**只有 `update` 明显低于 L1 速率**（它的目的按桌面
  行距跨行落，每行只用满一段）⇒ **再压就得压字节数**，把循环写窄没有用（见 8.5.3/8.5.4）。
- **compose**：`gdi_OutputUpdate` 的 surface→primary 合成；全屏会话里它已被"桌面镜像 surface 直接合成进
  primary"整段消掉（见 [`present-pipeline.md`](present-pipeline.md)）。
- **present**：绝大部分是**固定的 Vulkan 调用**（acquire / 录制 / submit / present），不是能删的活；
  `flush` 只有 µs 级。**GPU 侧另算**：`copy`（脏区字节 buffer→image）+ `blit`（clear 整张 + letterbox
  quad），**`blit` 与脏区无关**（见 [`present-pipeline.md`](present-pipeline.md) §6）。**这正是零拷贝
  后端（Vulkan）与 GLES 的差别**：前者 gdi 直接合成进呈现器缓冲（`copy` 是唯一一次搬运），后者必须在
  CPU 侧把脏区 memcpy 进 staging 再上传 ⇒ 两个后端不构成"脏区形状"的对照实验。
  - ⚠ **零拷贝下 `copy` 与帧序是耦合的**：它读的就是本帧要写的那块内存，所以排在**本帧第一次写之前**
    （`sync`）。脏区字节大时它直接进帧墙钟。判据：**`sync` 与 `uploaded` 同步起落**就是这一项，
    而不是"合成贵"。
- **`setup`（非像素命令）**：低频但可能很重，要按**"每次调用分配多少字节"**查。典型暗礁是
  `ResetGraphics` 按桌面尺寸重分配大块 scratch（且会对多个 codec 集合各做一遍）；现在几何未变就直接返回。
  ⚠ 表面对比那一半（`CreateSurface` 的 `memset(surface->data, 0xFF)`）**不能省**：未绘制区域必须是 0xFF。
  ⚠ 也不要靠"关掉 planar codec"来省（`gdi_SurfaceCommand_Planar` 会拿到空上下文）。

### 8.3 量测纪律与陷阱

- **频率会跟着回放节拍跑**：轻负载样本被压在最低频档、重负载跑满，同一份代码能差数倍。
  ⇒ `mode=fast`（dev 页「跑满」）**不打节拍**；复现 live 的到达节奏用 `realtime`；
  **判读前先看 `cpuKHz=`，不同就不要横向比**。
- **只有同一次会话里的 A/B 才有效**：把待测特性关掉各跑几轮取中位数；只看 `(running=0)` 的整轮。
  用 QoS 去修频率**无效**（实测毫无变化）。跑满后轻样本会顶到显示/GPU 上限 ⇒ **A/B 纯 CPU 侧的改动
  要用 CPU 受限的样本**。
- **计时器本身会被测出来**：本平台 `clock_gettime` 不是 vDSO 级 ⇒ **per-tile 计时这条路放弃**；
  要量只能"单线程墙钟差"或**按比例采样**（`prog2` 就是 1/16）。`CLOCK_THREAD_CPUTIME_ID` 在本平台
  **不可信**（累计值可超过整轮进程 CPU）。
  - `prog2` 的读法：把**占比**与 `dec` 一起看——1/16 采样会让 `sum` 比 `dec` 高几个百分点（样本偏斜 +
    缩放），这点差不是发现了新开销。
- **探针只做一次性实验、量完就删**；长期保留的只有 per-message 计时（`read/dispatch/dec/update`、
  `calls/unions/tiles/tilesDec`）、**逐相位探针 `prog2`**、app 侧 `setup` 计数、present 的 GPU 时间戳、
  以及解码侧的**对拍计数**（8.4）。
- **解码侧的 dev 计时按"谁展示"开关，不常开**：解码器里那组计时（`HmrdpProgStat` 的 `read/dispatch/dec/
  update` 与 `prog2` 相位、逐 tile 的 1/16 采样）只有**回放的 stats** 会读，live 工具栏不展示；本平台
  `clock_gettime` 不是 vDSO、采样计数器又是每 tile 一次共享原子加 ⇒ 本组探针由
  `HmrdpSetProgSample(on)` 控制，**默认关**：回放 CPU 路线在自己的轮次里打开，live 连接显式关闭。
  逆 DWT 对拍是另一档（`HmrdpSetDwtCheck`），只在 `参考:对比` 打开（8.4）。
  ⇒ 读 live 的 `本机`/拆相时，解码段里不含任何 dev 计时；`prog`/`prog2` 的数字只代表回放那一轮。
- **不要用"跳过某条 dispatch/步骤 + 差值反推"做归因**（依赖关系会变）：用计数器 + 相位桶。
- **回放节拍若落在 `gdi_EndFrame` 里，必须扣掉**（否则算进 `compose`）：`GfxWorkMeter::OnPace` 为此存在；
  `sync` 同理，**别把"等 GPU"当"合成贵"**。
- **相加之和不等于整轮 `cpu=` 时要分清**：`cpu=` 是整轮进程 CPU（含 ffrt 任务所在线程、也含平台侧记账），
  相位只是 RDP 线程。**串行时两者应当几乎相等**——不等就先查探针，别先怀疑算力。并行时多出来的是
  worker 的账——**不要用相位之和反推并行烧了多少电**，但可以用"`cpu=` − 串行那轮的 `cpu=`"估并行多烧的
  绝对量（旧执行器——FreeRDP 自带的 WinPR 池——这笔账可达解码段墙钟的十倍量级）。
- **墙钟反推的"GPU 时间"不算 GPU 时间**：只有时间戳量到的才算，其余是排队。
  present 的 GPU 时间有长期探针（`vulkan present probe: gpu copy=… blit=…`）：**`copy` 正比于脏区字节、
  `blit` 与脏区无关**。
- **多轮测试要标准化，不要"猜"**：每个 stats 文本的**第一行**是
  `state=running|finished|aborted  run=<n>`，`native/scripts/replay-rounds.ps1` 就是按这个写的：
  - 点一次 → 等**这一轮**的 `run` 变成 `finished`。不要用"现在好像没在跑"来判断：两轮的 stats 文本
    长得一样，按内容猜必然把数据记到上一轮头上（`run=` 就是为此存在的）。
  - **一轮没跑完绝不点下一次**：dev 页的模式按钮（参考/节拍/路线）内部都是 `stop + start`，
    中途点击 = 掐断，而**掐断的轮次不是测量**（驱动会直接把 `state=aborted` 报出来）。
  - 掐断的轮次**不写参考**（导出只在完整轮次落盘），否则一次半截导出会把好参考换成"跑了十几帧的哈希表"。
  - 脚本负责"设模式 → 推录像/参考 → 点重新回放 → 收 stats"，落到 `rounds-*.txt`；人只看文件。
- **dev 页驱动：别用盲点坐标**。先 `dumpLayout` 取控件 `bounds` 再点中心
  （见 [`build-and-verify.md`](build-and-verify.md) §5.1）；**按钮在顶栏（y 很小），而状态行会重复同名
  文字**（如 `路线:CPU`），按 `text` 找控件时要取顶栏那个。
  - ⚠ 驱动脚本用坐标点击，所以**窗口必须在最前**：应用被系统重启/窗口被切走时，点击会落到桌面上
    （症状：点了没反应，脚本会报"click did not start a new run"）。脚本因此先 `aa start` 把页面拉起来。

### 8.4 正确性门禁：参考画面 + 解码侧对拍

- **门禁 = 参考画面（golden reference）**，两份基线录像各一套：先 `参考:导出` 记下
  `hmrdp_ref_<captureTag>.{hash,bmp}`，之后每轮 `参考:对比` 报 `bad=0`。参考文件名带**录制内容指纹**，
  换一份录像必然缺参考、必须重新导出——这就是"同名文件的不同录制不能互相背书"的机器保证。
  - ⚠ **它是"输出不变"的门禁，不是"与旧解码器逐位一致"**：解码侧允许**舍入级**改动（`WITH_SIMD` 这类
    SIMD 实现不保证与通用 C 逐位相同），但**必须把差异量出来**：系数/像素最大偏差 **1 量级**、差异像素
    占比**千分之几** = 舍入，可以收；**成百上千** = 实现不同（16 位回绕、饱和当回绕、错索引），必须改回去。
    ⇒ **口径一变就要重录参考**，此后 `bad=0` 只表示"自那次重录起没有再变"（回归门），量级账由下一行承担。
  - ⚠ **参考证明不了"解码侧改动的量级"**（参考就是被改的那个解码器录的，两边一起变）：解码侧改动必须自带
    **对拍**——patch 里 `HmrdpDwtCheckStat[3]` = `{对拍过的 tile 数, 逐元素不同的个数, 最大 |Δ|}`，
    `参考:对比` 那一轮按 1/16 采样打开，stats 行报 `dwt check: tiles=… mismatch=… maxDelta=…`。
    - **判据是 `maxDelta`**：`0` = 逐位等价（最理想，参考可直接复用）；个位数 = 舍入；成百上千 = 实现不同。
    - 对拍要挂在**真正跑的那一支**上：`-DWITH_SIMD=ON` 时抽取支是 `codec/neon/rfx_neon.c` 的
      `rfx_dwt_2d_extrapolate_decode_neon`，逐位参照是 `HmrdpDwtExtrapolateReference()`（上游标量实现）；
      两边都要挂，否则会看到 `tiles=0` 这种"没对拍过"的假通过。
    - 取输入副本必须在**解码之前**：逆 DWT 的输出就写在系数缓冲上（4096 个系数全被覆盖），解码之后再复制
      拿到的是输出，两边算的不是同一件事。
    - **像素级量级**由 `参考:对比` 给：`rgbPx`（差异像素数）+ `maxDelta`（通道最大偏差）。两者都只看量级。
  - **逐像素诊断**（"差在哪一帧、差多少"）：`rgbPx/alphaPx/maxDelta/bbox` 只覆盖参考图像那一帧；要逐帧
    定位就重新 `参考:导出` 再比。
  - 参考模式**不是性能模式**：每帧对整个桌面做一次哈希（`hashMs=`），这段时间落在 `EndFrame` 里、会算进
    `compose` ⇒ 读性能数字请用 `参考:关` 那一轮。
- **上游 NEON 的边界**：`codec/neon/rfx_neon.c` 的两支逆 DWT 都在**16 位车道上做加法**（`(a+b+1)>>1`
  与 `>>1`）——|a+b| 超过 int16 时会**回绕**，那不是舍入。实测本工程码流上从未触发（对拍 `maxDelta=1`
  恒成立），因此抽取支采用上游 NEON；**通用支仍用逐位等价实现**（它在 Progressive 码流上一次也不进）。
- **`bad=0` 覆盖不到的顺序约束**：上屏侧的帧序/等待（零拷贝单缓冲）不属于像素内容，
  判定只能靠机制 + 计数器（见 [`present-pipeline.md`](present-pipeline.md) §4）。

### 8.5 被否证的假设（成本该按什么估）

1. **"每次 tile 的 region16 union（O(n²)）是 `update` 的大头"** —— 先把相邻 tile 索引合并再 union，
   `unions ≈ tiles`（Progressive 每条消息的 region 基本就是每 tile 一个矩形，没有可合并对象）⇒ 零收益；
   再改成"每 tile 行一个 span、帧末一次并进 region"，`unions` 降了 50 倍以上而 `update` 只降 ~0.5ms
   ⇒ **union 总共只值 ~0.5ms**。   ⇒ 教训：**`update` 的账按"拷贝字节数"估，不要按"矩形条数"估**；而这份字节成本**落在哪条线程上**
   比它的多少更值钱（拷贝现在落在 tile 解码段上，见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §4）。
2. **"delta 折叠（只拷没写过的区域）"** —— 去重 stamp 实测命中 **0 次**（帧内没有重复合成）⇒ 无收益。
3. **"`state` 的饱和加是算力瓶颈"** —— 那个循环按元素做两次分支钳位，看着像算力账；换成 `VQADD`
   （8 路、逐位等价的饱和加）后 `state` 在噪声内 ⇒ 它**受搬的字节数限制**（工作缓冲与持久 `current`
   两个缓冲都要写），不是受钳位限制。要降它得**少写一趟**，换 SIMD 不解决问题。
4. **"`update` 的 keep-alpha 拷贝是算力瓶颈"** —— 换成 4 像素/指令的字节掩码选择（逐位等价）后
   `update`/`本机` 都在噪声内 ⇒ 这个循环同样受字节数限制（16KB 读源 + 16KB 读目的 + 16KB 写目的，
   目的跨行落）。要动它只能动**字节数**（例如"目的 alpha 恒为 0xFF 时省掉那次读"，但那要求先证明该表面
   从不出现非 0xFF 的 alpha——`KEEP_DST_ALPHA` 是上游语义、alpha 也在参考哈希里，属**改语义**，
   不在"舍入"的许可范围内）。
   - ⚠ 该实验的**判据没当场证实**（`NEON_INTRINSICS_ENABLED` 依赖 CMake 生成的 `config.h`，
     "NEON 版真的编进去了"没有被独立验证）⇒ **这类实验要先证明"改动确实生效"**（打一个可观测计数），
     否则"中性"可能只是"没跑"。
   - ⇒ 教训：**"看着像算力"的循环，先用一个逐位等价的 SIMD 版本试一次**——它是中性的就说明账在内存侧，
     该动的是数据流而不是指令。
5. **"按预测的线程速度预先分配 home"** —— 移动 SoC 上线程间的吞吐确实差得大（最快/最慢 2 倍量级，
   同一线程的快慢排序也大致稳定），但**偷取本来就主要发生在子线程之间、并已按速度把 tile 分出去**：
   预先分配只改**起点**，实测对偷取占比没有显著改善。⇒ 划分继续用"等分 home + 段尾块偷取"，
   **不要再引入线程速度预测/加权划分**（要动划分，先看 §8.6 的规模自适应那一条）。

### 8.6 已定型的约束（decode 侧）

- **`update_tiles` 的记账形状已定型**：脏区按**每 tile 行一个 span** 记（帧末并进 region16），逐 tile 不再
  建 region16；这个形状本身只值 ~0.5ms/帧 量级，不要再去合并矩形或换 region 结构（两次实测收益都在噪声
   里，见 8.5.1）。**该段真正的成本是像素拷贝**，而它默认已随 tile 解码段并行
   （[`cpu-accel-plan.md`](cpu-accel-plan.md) §4）。
- **`sign` / `current` 是两个持久状态**：`sign` 存 RLGR 原始系数、`current` 存去量化后的系数，
  两者跨消息常驻（UPGRADE 按 `sign` 判符号、DIFFERENCE 按 `current` 累加）⇒ 任何"少写一趟"的改动都要
  保证这两个缓冲最终内容不变（逐位等价），否则对拍会立刻显示出来。
- **上屏侧的约束**（脏区形状、零拷贝桌面缓冲、等待点）见
  [`present-pipeline.md`](present-pipeline.md)。
- **规模自适应只有一个点**：region 太小就降档甚至串行（`HMRDP_MIN_TILES_PER_WORKER`，
  [`cpu-accel-plan.md`](cpu-accel-plan.md) §2）——小 region 上多开线程的收益低于每 region 的
  提交/唤醒成本。
