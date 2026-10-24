# Moonlight 视频（解码 / 上屏 / 编解码协商）

本文件是 Moonlight 视频这一段的家：解码（HarmonyOS AVCodec）、两条上屏链路、编解码协商与 `STREAM_CONFIGURATION`
字段落点。原生库构建与解码桥接线见 [`moonlight-native.md`](moonlight-native.md)；控制/配对见
[`moonlight-control.md`](moonlight-control.md)；会话窗口与遥测见 [`moonlight-session.md`](moonlight-session.md)。

## 1. 解码（AVCodec）

解码用 HarmonyOS AVCodec（`OH_VideoDecoder_*`，库 `libnative_media_vdec.so` / `libnative_media_core.so` /
`libnative_media_codecbase.so`）；`hmrdp_video_decoder` 一个实例包一个硬件解码器，输入是 **Annex-B 字节流**；
**一次 Submit 必须是一个完整的访问单元（一帧的全部 slice + 其前置 SPS/PPS/SEI）**——AVCodec 的硬性约束是
「同一帧的多个 slice 需一次送入解码器」，把一帧拆到多次 Push 会被当作损坏输入，解出来的画面即花屏/错乱。
Moonlight 的拆包器本就把一帧的所有 NAL 交给一次回调（见 [`moonlight-native.md`](moonlight-native.md) §2 的
解码桥），所以产品链路天生满足这个约束。`video/avc` = H.264，`video/hevc` = HEVC。

- **解码器配置口径**：Configure 的宽高是**解码器实例的最大分辨率**（SDK 据此分配资源），**不是**上屏尺寸：
  不要拿窗口/surface 尺寸当提示（会为远小于它的码流过度分配），会话侧按协商的流分辨率设。**实际可见尺寸**
  由 `onStreamChanged` 的 `OH_MD_KEY_VIDEO_PIC_WIDTH/HEIGHT` 给出，只作解码器的可见尺寸参考；它**不**改组件
  几何（组件比例取请求的流分辨率，见 §2）。
- **帧率进 Configure，不置可变帧率开关**：`OH_MD_KEY_FRAME_RATE` 取协商帧率（键声明为 double、示例传
  int，故按 setter 的类型校验择一设置）；越界会让 Configure 回 `AV_ERR_INVALID_VAL`，故失败时**不带帧率
  重试一次**。`OH_MD_KEY_VIDEO_DECODER_OUTPUT_ENABLE_VRR` 不置：这里**不需要**它——显示端吃不下由送显槽位
  自己处置（等槽位、被新帧接管），平台那套刷新率自适应不是链路的必要条件；协议侧也没有这一项（Moonlight 按协商 `maxFPS` 送帧，
  没有刷新率握手），故它不构成一个可加的开关。
- **送显节拍（等到槽位，不硬丢）**：面板一个刷新周期只能显示一帧，多余的那部分必须有人处置。buffer
  （超分）链路把它交给**交接线程**：帧先等到自己的槽位再送呈现器，**等待期间来了更新的帧就由新帧接管
  同一个槽位**（被顶掉的旧帧当场还给解码器）。所以"被顶掉"的那些帧正是面板本来显示不了的那些，而每个
  刷新周期显出的都是此刻最新的一帧——不在解码回调上 sleep、不建时间轴、不请关键帧，也不会延迟堆积。
  **等待上限就是一个刷新周期**（槽位最远只在一个周期之后，`SlotSchedule::NextSlotUs`），所以这项等待既
  不需要额外预算，也不改变端到端延迟。面板档位与槽位判据取自共享的 `hmrdp_display_rate.*`（档位取会话
  窗口所在显示设备，见 [`present.md`](present.md) §2；读不到刷新率时不判槽、不等待）。
  桌面链路（RDP）在同一判据下的处置是**合并**而不是等待，见 [`rdp-present.md`](rdp-present.md) §3。
  surface 链路（见 §2，非超分的常态路径）没有可等待的线程，因此维持"太早就释放"的硬丢。
- **模式只按"是否要超分"定、运行期只报错**：请求了超分的会话问一次 `FramePresenter::supportsVideo()`
  （呈现判定 + 视频导入判定），成立就走 buffer 链路，否则静默回退到 surface。**非超分会话（HDR 也是其一）
  一律走 surface**，根本不建呈现器，也不进这条判定。**输出模式不能中途切换**（换模式要
  重建解码器，见 [`present.md`](present.md) §1），所以运行期一旦持续拒绝帧（buffer 路：导入失败、缓冲布局或
  像素格式读不出来、fence 超时；surface 路：输出表面连续拒绝渲染）就是**黑屏**：连续 4 次即确认，报
  `video-present`，会话页以**可停留的
  Notice** 告知（关键错误不能只靠 Toast，见 [`ui-feedback.md`](ui-feedback.md) §1.5），文案指向"换编码或
  降低分辨率重连"，不再指向超分。
- **硬解优先，软解兜底，无解器报错**：`VideoDecoderSupport()`（`OH_AVCodec_GetCapabilityByCategory`）把每种
  编码分成**硬解 / 软解 / 无**，优先创建硬解，硬解缺失时才退到软解，`VideoDecoder::hardware()` 反映实际
  选择；完全没有解码器时起播同步失败并给出原因，绝不静默黑屏。
- **送显即出帧，客户端不排程**：surface 模式解码出帧就送显（吃不下由上一条的闸门丢），节拍由显示器 vsync
  与主机按协商 `maxFPS` 的发送共同决定。客户端自建时间轴（帧计数配协商帧率 + 首帧墙钟推
  `RenderOutputBufferAtTime`）不采用：与码流时间无关，主机实际帧率不等于协商值时偏差只累加、不回正，表现为
  静默的延迟堆积；且 SDK 约定输出 buffer **顺序处理**，被按住的 buffer 会阻塞后续。
- **输入按延迟预算排队，持续超预算才丢积压并请主机补关键帧**：解码桥不把提交透传给 codec，而是入一个按**时间**
  （不是字节）计量的队列——预算 = 3 个帧周期、钳在 30–150ms，帧率未知时 75ms。**超预算不等于解码落后**：网络把
  一簇帧一次送达（以及解码器随后把它们消化掉）也会把队列堆起来，而那一簇帧仍在参考链上——为它们丢掉整段积压
  等于用一次关键帧往返去修一个**送显闸门本来就处理得了**的延迟（闸门每个刷新周期只放一帧、且取最新，见 §2）。
  所以判据是**时间**：积压**过了预算再维持一个预算**（≈ 2 倍预算的等待）才算「解码器在丢阵地」，此时才
  **丢掉整段积压**并回 `DR_NEED_IDR`——库收到该返回值后清掉自己的分帧状态、停止送帧直到新 IDR 到达，所以一次
  真过载只花一个关键帧就回到基线延迟，不会形成关键帧风暴（限流由库侧完成）。被保留的那簇只记一次日志
  （`input … behind budget and still draining; keeping the frames`），随后照常解码、由闸门决定显示哪几帧。
  运行期**不存在**任何以字节数封顶/只拒新帧不清积压的路径——那会把「跟不上」固化成永不恢复的延迟堆积，
  而字节数不是延迟的量纲。
- **低时延输出 = 解码序，故码流不得含 B 帧**：Configure 使能 `OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY` 后解码器
  **按解码序输出、不做重排**（交互场景要的正是这个低延迟）。送显时间轴因此就是解码序，只有码流**不含 B 帧**
  时解码序才等于显示序。Moonlight / Sunshine 的编码本就是低延迟无 B 帧，产品链路成立；换成带 B 帧的码流
  会以错误时序上屏（看着像跳帧、帧计数虚高），那是输入不符合该前提，不是解码缺陷。
- **无每帧解码耗时接口**：`OH_AVCodec` 的 VideoDecoder 只回 `OH_AVCodecBufferAttr`
  （`offset`/`size`/`pts`/`flags`），**不报每帧解码耗时**；带统计的 `OH_AVPlayer_GetPlaybackStatisticMetrics`
  是解封装播放器口径，对直连 VideoDecoder 不适用。故工具栏「本机」只能取**提交 → 解码出帧的往返**
  （`submitDecodeUnit` 记提交时刻、输出回调按 `pts` 配对求差），它**含输入排队与输出缓冲等待**，是纯解码
  耗时的**上界**——解码器空闲时接近解码耗时，积压时被放大；有了上一条的输入预算，这个放大被钳在
  「预算 + 解码耗时」以内，读数不再能无限膨胀。判读与着色见
  [`moonlight-session.md`](moonlight-session.md) §2。
- **解码有两种输出模式，按"帧要不要交给 GPU 采样"选**：**surface 模式**（`OH_VideoDecoder_SetSurface`）把解码帧
  直接写进给定的窗口，只送显、应用侧不碰像素；**buffer 模式**（不设输出表面）由解码器交出自己的输出缓冲，帧经
  `OH_AVBuffer_GetNativeBuffer` 成为**图形分配器**的 `OH_NativeBuffer`，可交给 GPU 采样。两种模式的**布局信息
  可得性不同**：buffer 模式在**流变化事件或 `OH_VideoDecoder_GetOutputDescription`** 里报出字节行距与单面的
  已分配行数，surface 模式不报（其内部数据走 HEBC，`OH_MD_KEY_VIDEO_STRIDE`/`OH_MD_KEY_VIDEO_SLICE_HEIGHT`
  取不到）——所以凡是按平面字节采样缓冲的链路只能走 buffer 模式，只送显的链路用 surface 模式。
  `OH_AVBuffer_GetAddr`（CPU 地址，逐行搬运用）本仓不使用。几何取法与导入配方见 [`present.md`](present.md) §4。
- **buffer（超分）链路上屏侧只有一个丢弃点**：待交接队列只留最新一帧，被新帧追上时**旧的那帧当场归还**
  （`handOffReplaced`）——那正是"面板一个周期显示不了它"这一件事，不需要在解码回调里另判一次槽位。
  已提交给呈现器的帧最多两帧在飞，各自等 fence 后才归还，所以"上屏跟不上"的代价是丢帧，不是延迟堆积。
  另有**输入预算丢弃**（解码自身的积压，见上一条），与面板无关。  两处丢弃都进遥测，加上 surface 链路的
  闸门丢弃（`presentDropped`，buffer 路恒 0）构成总丢弃率（见
  [`moonlight-session.md`](moonlight-session.md) §2）。其中**只有丢弃发生时面板当前档位低于协商帧率的那
  部分**才算「屏幕吃不下」，单列为 `refreshDroppedFrames`：判据是共享的 `DisplayBelowFrameRate()`
  （`hmrdp_display_rate.*`），**每个丢弃点各自判一次**，所以它是链无关的那个数。

- **原始串流录制（开发用）**：库内采集钩子（`LiSetVideoCapture`）在每个访问单元交给渲染回调之前把它落盘成
  `hmrdp_ml.bin`（含 PTS/到达时刻/颜色与 HDR 事实），用于离线分析/复现（如 HDR10 相对 HLG 的亮度闪烁）；
  采集点、格式与开关见 [`moonlight-native.md`](moonlight-native.md) §2.6。

## 2. 上屏：超分走 buffer 呈现器，其余走解码 surface

**唯一的常态链路是解码器自带的输出表面（surface 模式）**：解码帧直接进会话窗口，画面与元数据都由平台处理
（HDR 的 HDR Vivid 播放方式就在这条路上，见本节末）。**只有请求了超分的会话才走 buffer 模式**：解码交出输出
缓冲，经 Vulkan 呈现器导入、转成 RGB、上采样后上屏——超分是呈现器存在的原因本身，不是它上面的一个可选段。
呈现器不能导入缓冲时静默回退到 surface（与 RDP 的 GLES 兜底同级：不参与送显节拍，判槽即丢）。
**模式在会话开始时一次性判定，运行期不可切换**（换模式要重建解码器），所以运行期"这帧呈现不了"
只能报错、不能改走另一条路（见 §1 末两条）。

- **超分：解码走 buffer 模式 + Vulkan 导入，复用 RDP 呈现器**。会话按**输出分辨率 ÷ 倍率**取流
  （`/launch` 与 `STREAM_CONFIGURATION` 的尺寸都取会话分辨率，fps 钳制与自动码率也按它算，见 §4；不开超分时
  两者相等），本机每帧经呈现器上屏，开了超分再上采样回输出分辨率。解码器**不设输出表面**，每帧把自己的
  输出缓冲交出来（`OH_AVBuffer_GetNativeBuffer`）；解码桥在交接锁下把该缓冲交给
  `VkRenderer::PresentVideo`；提交成功时返回该帧的呈现槽位 token，**GPU 读完之前不归还**给解码器——
  交接线程最多留两帧，`WaitVideoFrame(token)` 等前一个槽位的 fence 后才把缓冲交回 codec，因此
  解码/convert 与上一帧的上采样可以重叠。`core/hmrdp_vk_video.*` 负责导入与 convert pass
  （整块按 **R8 字节图**外部内存导入，片元 `texelFetch` 取 Y 与交错 Cb/Cr、**自己做 YUV→RGB**），写进
  **会话分辨率的 picture**（每帧槽一份 picture/UBO），之后接既有的超分 / letterbox / swapchain。
  **导入配方、几何取法（取解码器报的账，不是分配器的推断）、归还与门控的判据全在
  [`present.md`](present.md) §4**——本节只讲本仓怎么接。因为像素与色彩空间都经我们自己的 pass，
  会话把协商的 CSC 交给呈现器（`SetVideoColorSpace`）做核对，不一致报 `color-mismatch`。
  此链路**只在真机验证**（Vulkan 呈现本就真机），且要求设备支持视频导入（见 [`present.md`](present.md) §5）；
  不满足时走下面这条 surface 链路。
- **HDR 也是"非超分"的一例：走解码 surface**。请求 HDR（`colorSpace == Rec2020`）的会话**不建 presenter**
  （因此也不叠超分），由解码器直出会话窗口。这里**完全按平台自己的 HDR Vivid 解码播放方式**做，本仓不在窗口上
  声明任何东西（不设格式 / 色彩空间 / 元数据类型，也不要求解码器"以 surface 为准"）：**帧与元数据都由解码器产出**，
  我们只喂码流、`RenderOutputBuffer` 送显。这是刻意的——只有当我们自己什么都没多说，设备上仍然出现异常，才能
  把结论落在平台那条路上；我们多说的每一句都会让结论不可归因。（这条取向的由来：HDR10+Vivid 在自建 Vulkan 上屏
  与这条官方 surface 路上**表现一致地闪烁**，而 HLG+Vivid 正常，于是把这一路收回官方做法。**"落在平台侧"并不是
  终局**：真正的原因是主机写进码流的那份动态元数据里多了一条会跳的曲线，见本节末三条。）
  解码器照旧**先声明**这段流的编码（`OH_MD_KEY_COLOR_PRIMARIES` / `TRANSFER_CHARACTERISTICS` /
  `MATRIX_COEFFICIENTS` / `RANGE_FLAG`，H.273 码点：SDR 按协商的 CSC 标 BT.709/BT.601 + gamma22，HDR 标
  Rec.2020 NCL + PQ/HLG + limited）——码流 VUI 缺失或不对时，声明才是权威口径（平台这几个键的默认是
  UNSPECIFIED/limited）。传入的编码由连接的 `色彩协议与HDR` 决定（`MoonlightVideoSetStreamColorSpace`，与协商的
  `colorSpace`/`colorRange` 同源）。
  **HDR 内容亮度只有一条通道**（设置 `HDR 内容亮度`，0–100%）：ArkUI 的 `XComponent.hdrBrightness`
  （平台给"组件显示 HDR 内容时的亮度"的正规接口，**实机已验证生效**）。`0` = 按 SDR 亮度显示、`1` = 按当前允许
  的最高 HDR 亮度，而 `1.0`/`100%` 就是平台默认值，所以本项的默认值也取 100（等于不动默认观感，需要时才下调）。
  **它只在真的在放 HDR 时生效**：会话页按 `moonlightVideoHdr()`（主机已确认）给值，非 HDR 恒为平台默认 1.0，
  所以"请求了 HDR 但主机没切"的 SDR 会话不会被它压暗。**它作用于本机显示的 HDR 内容**，与 §4 那组 `/launch`
  亮度上报（作用在主机侧）互不相干。实机核对的锚点：`hdr picture brightness N%`。
- **动态 HDR 只有 HDR Vivid**。这块客户端的定位就是**一块"HDR10 / HLG / HDR Vivid"的显示器**：平台的 HDR 模型
  就是这三种（`OH_NativeBuffer_MetadataType`），没有 HDR10+ 的类型与 API，所以动态层只认 HDR Vivid，
  流的其余内容按底座传输当普通 HDR10 / HLG 放。
  动态层与底座**正交**，不经任何协商：主机只要 HDR 亮度分析开着（默认 `auto`），就会把 Vivid 作为
  **注册 T.35 SEI** 写进普通 HEVC 码流（Vivid 无 AV1 承载，故自动档把 HEVC 排在 AV1 前，§3）。SDP 里**一个动态
  能力位都不发**——foundation-sunshine 的 `dynamicHdrCaps` / `dynamicHdrPreference` / `X-SS-Dynamic-HDR` 只服务
  PyroWave 那条本端没接的链路，补丁里也没有这几项。**前提是分包器别把它删掉**：common-c 默认在
  `VideoDepacketizer.c` 把前置 SEI NAL 无条件剥掉，本仓补丁加了 `CAPABILITY_PRESERVE_HEVC_SEI`（`0x80`）并设在
  解码回调表里（见 [`moonlight-native.md`](moonlight-native.md) §2），否则整条链路上根本看不到 T.35。
  `core/hmrdp_hdr_dynamic.*` 在每个访问单元里扫出 CUVA（`26 00 04 00 05`）的注册载荷并去 emulation-prevention；
  **只认这一种**——扫描不受同单元其它载荷的先后影响。HDR 由平台的解码路径呈现，动态层由它自己随帧取出，本侧
  不向平台声明任何动态层；扫描结果只服务两件事：工具栏的命名（`Hdr()` 把基座传输与动态层拼成 `HDR10 Vivid` /
  `HLG Vivid`），以及把下面那条不属于本端的载荷丢掉。日志锚点：`moonlight video: HDR Vivid detected`。
- **HDR10+ 由本侧丢掉**。基地版的约定是：PQ 会话里分析器把 **HDR10+**（`B5 00 3C` 注册 T.35，ST 2094-40）与
  Vivid **一起**写进每个访问单元（HLG 只有 Vivid；离线录像实测 626/626 个 PQ 单元都是"HDR10+ 一条 SEI NAL 在前、
  Vivid 一条在后"）。**本端没有商量余地**：发不发只由 `formats_for()` 决定，它只看色彩空间+编码（PQ 恒
  `{hdr10plus=true, vivid=true}`），客户端的 `dynamicHdrCaps` 到不了 NVENC/AMF 这条路，协商本身最多也只能给出
  HDR10+ 或"无动态层"——**没有"只留 Vivid"这个结果**，所以只能本侧动手。`PlanHdr10PlusRemoval()` 认"整条 NAL
  只装 HDR10+ 载荷"的前置 SEI NAL，`MoonlightVideoRenderer::Submit` 在交给解码器前把它从自己那份访问单元拷贝里
  删掉（删的是整条 NAL，长度会变；一条 NAL 里若还有本端要留的注册载荷就整条不动）。**留下的就是 HDR10 基线 +
  Vivid**：静态 MDCV / CLL 是另一条 SEI（`137` / `144`），一条都不动，所以 PQ 会话出去的形状与 HLG 会话只差传输
  函数。平台没有 HDR10+ 这个类型，实测这一步对本机画面也**没有影响**——它是语义上的正确性，不是闪烁的解法。
  日志锚点 `moonlight video: dropped the host's HDR10+ SEI`。
- **Vivid 载荷里的 tone-mapping 曲线由本侧抹掉**。录制时那份基地版给 PQ 的 Vivid 带了一条**逐帧曲线**
  （`tone_mapping_mode_flag=1` 加 base 参数：`targeted_system_display_maximum_luminance`、`m_p`/`m_m`/`m_a`/`m_b`/
  `m_n`、`k1..k3`、`Delta`）。实测它的中间调参数 `m_m` 只取 1.2 / 4.4 两个值、以 4 帧为量子来回切（626 帧切
  52 次，连续段 4~64 帧），且与 Vivid 自己的统计量无关（两组统计量均值几乎相同）——**这条曲线在本机这条路上
  唯一可见的作用就是"每几帧换一次映射"**，既不是由内容连续算出来的映射，也不是这里需要的东西（本机真正起作用的
  是统计量：HLG 只有统计量、不闪）。标准这边也要求这份数据在时间上稳定：GB/T 46269.1-2025 Annex A.9 建议对
  生成的动态元数据做 32 帧算术平均。`PlanVividCurveStrip()` 把 CUVA 载荷重写成 statistics-only：保留信封与 4 个
  12 位统计量，丢掉其后的曲线与 target 字段——也就是基地版**现在**的序列化器写出的形状、HLG 已经在用的形状；
  `Submit` 在交给解码器前替换掉自己那份拷贝（长度会变；已经是 statistics-only 的载荷不动，故幂等）。日志锚点
  `moonlight video: dropped the host's Vivid tone-mapping curve`。AV1 的 OBU 承载同样暂不处理（本侧只为 HDR 选
  AV1 于设备解不了 HEVC Main10 时，而有这个约定的主机一定提供 HEVC Main10）。
- **这一段（HDR10+Vivid 闪烁）的最终结论**：闪烁由主机 Vivid 载荷里那条曲线引起——本侧抹掉后实机**不再闪烁**。
  两条静态元数据（非法的 MDCV、与 Vivid 同发的 HDR10+）都实测修掉后照旧闪，平台与自建呈现也因此都无责。根因的
  修法在主机侧：**不写曲线**（基地版现在的序列化器就是这样）或**对曲线参数做时间平滑**；本侧这两步只是幂等兜底，
  主机改好后自动无操作。
- **解码直出 XComponent（非超分的常态路径，也是超分不可用时的兜底）**。surface 模式（`OH_VideoDecoder_SetSurface(codec, window)`，必须在
  `Prepare` 之前）把解码帧直接写进会话页 XComponent 的 `OHNativeWindow`。应用侧**没有呈现器**、没有 GPU
  合成 pass；留边由**画面组件几何**承担——会话页用 `components/SessionPicture` 把 surface 约束成**流分辨率
  比例并居中**（与 RDP 同一套 `utils/PictureFit`），组件比例恒等于内容比例，合成器在窗口拖拽时只做等比缩放，
  **无需重绘**。窗口另设 `OH_NativeWindow_NativeWindowSetScalingModeV2(window, OH_SCALING_MODE_SCALE_FIT_V2)`
  （不设则默认拉伸填满），兜住协商流分辨率与请求值不一致的情形。留边区域**不接收指针**：输入/焦点在 XComponent
  上，事件坐标即画面局部坐标，映射是整画面归一化。它是平台文档推荐的视频播放方式（延迟/功耗最优，全屏原生
  比例时可能命中硬件直通/overlay），但应用侧对送显没有控制权：解码输出回调上只能"送或丢"，没有可等待的
  线程（见 §1），因此它不参与送显节拍，也**不承载超分**（拿不到输出图）——会话请求了超分却没有呈现器时，
  按输出分辨率取流由主机放大。

## 3. 编解码协商

支持的编码是 H.264 / HEVC / AV1（`video/avc` / `video/hevc` / `video/av1`）。
协商**由客户端单向决定、主机只广告不自选**：主机在 RTSP `DESCRIBE` 的 SDP 里列出**能编码的全部格式**
（Sunshine 用 `sprop-parameter-sets=AAAAAU` 表示 HEVC、`AV1/90000` 表示 AV1），客户端库在本机
`supportedVideoFormats` 与该广告的交集上按固定优先级 `HEVC > AV1 > H.264` 取一，再用 `ANNOUNCE` 的
`x-nv-vqos[0].bitStreamFormat` 回报。**决策权不交给库**：`moonlightVideoFormats(choice, serverCodecModeSupport)`
直接产出**恰好一个** `VIDEO_FORMAT_*`（非能力集合），库只负责照此回报：

- **自动**（`ML_CODEC_AUTO`）：硬件档 `HEVC→AV1→H264`、无硬解再软件档同序逐级下退，取第一个「本机该档可解
  且主机可编码」的。H.264 不查主机（库的兜底就是它）；AV1 / HEVC 要求 `SCM_MASK_AV1` / `SCM_MASK_HEVC`——
  Sunshine 的 `SCM` 位与 `DESCRIBE` 广告同源（`active_*_mode >= 2` ⟹ `!= 1`），故选中者必被库确认。
- **手动**（连接编辑页 / 设置页的编码选择，默认自动、可会话独立覆盖）：**强制**所选编码，同样要求本机可解
  且主机可编码。选择项按 `moonlightVideoCodecSupport()`（硬解 / 软解 / 无）置灰，仅软解时下方红字提示无硬件
  解码支持，无解码器时提示无法串流。
- **HDR 只取 10 位档位**：`色彩协议与HDR` 项选 Rec2020（HDR）时，`moonlightVideoFormats(choice, scm, hdr)` 在
  `HEVC Main10 > AV1 Main10` 里取一个（HEVC 在前：Vivid 只由 HEVC 承载，本平台 AV1 支持也偏弱），要求本机该 10
  位 profile 可解 **且**主机广告了
  `SCM_AV1_MAIN10` / `SCM_HEVC_MAIN10`（家族位 `SCM_MASK_*` 不够）；H.264 载不了 HDR，不参与。取不到就
  **显式失败**（不悄悄降成 SDR 编码），文案指向"需要 HEVC Main10 或 AV1 Main10"。**这一项是协议选择，不按
  显示能力置灰**——只按协议可行性（本机能否解 10 位）；能不能显示由运行期决定，显示不了就降级（§2）。
- **无共同编码即显式失败**：`moonlightVideoFormats` 返回 `0` 时 ArkTS 在 `LiStartConnection` 前直接失败；
  会话中若主机协商出非决策编码，`setup` 拒绝（`codec-mismatch`），不静默改推别的编码。
- **丢帧恢复的形状由声明的能力位决定，且不按协商结果挑**：`CAPABILITY_REFERENCE_FRAME_INVALIDATION_*` 声明的是
  「本机解码器能正确吃未经改写、DPB 更深、带部分失效参考的码流」，按编码族、条件是该族有硬解（见
  [`moonlight-native.md`](moonlight-native.md) §2）。它直接改变 SDP 的 `x-nv-video[0].maxNumReferenceFrames`：
  **没声明 → `1`**（每帧只参考前一帧，丢一帧即整条链失效，只能整段等 IDR）；**声明了 → `0`**（主机自选，实测
  NVENC 是 `DPB=5`、AV1 `8`，而**每帧仍只用 1 个参考**——基地版 `set_ref_frames()` 的原话），于是丢帧由主机
  **失效那几帧的参考**来补，客户端丢 1–2 帧后继续解码而不是整段等关键帧（NVENC/AMF 走
  `nvEncInvalidateRefFrames`，软件编码器回落成 IDR）。**两端都满足才生效**：主机没广告
  `x-nv-video[0].refPicInvalidation` 时库自动回落到等 IDR，所以最坏情况只是「没赚到」、恢复形状不会更差。
  效果读数是工具栏的 `补帧`（见 [`moonlight-session.md`](moonlight-session.md) §2）。

协商结果只有软解时以 `software-decode|<mime>` 警告（提示主机换编码器）；运行期另有 `sr-unavailable`
（会话请求了超分但没有可用呈现器）、`sr-inactive`（呈现器在跑但上采样未生效）、`video-present`
（连续拒绝帧 ⇒ **确认黑屏**：buffer 路由呈现器报告，surface 路由解码器连续被拒的渲染报告，见 §1）与
`color-mismatch`（解码缓冲声明的色彩空间与协商的 CSC 不同族或不同范围，见 [`present.md`](present.md) §4）
四种警告，多个事实以逗号分隔。**`hdr-degraded` 已不存在**：HDR 走解码 surface 后，画面怎么被呈现全在平台那一侧，
本机没有任何可读的判据说"主机在 HDR 而本机没在跑 HDR"，因此不再编造这个状态（这正是"只说自己知道的事"）。
`moonlightVideoCodec()` 给出编码标记（h264/hevc/av1），工具栏把它与超分状态合并进
分辨率后的**同一个括号**、逗号分隔（`1920 × 1080 (FSR 1.5x, HEVC, HDR10 Vivid)`；软解再加 `CPU`；超分未生效/导入失败
另有 `未超分`/`视频异常` token，色彩对不上另有 `色彩存疑`，HDR 那项只在**真的在放 HDR 时**才出现——判据是
`moonlightVideoHdr()`，即主机已确认 HDR（名字是基座传输，动态层在时追加 `Vivid`），见
[`moonlight-session.md`](moonlight-session.md) §2）。

## 4. 其余 `STREAM_CONFIGURATION` 字段的落点

- `colorSpace` / `colorRange` 合成 SDP `x-nv-video[0].encoderCscMode`（`(space<<1)|range`）交主机做编码 CSC——
  `0` 是 Rec601（HD 下偏色）、`1` 是 Rec709（SDR 默认）、**`2` 是 Rec2020（HDR）**。这就是`色彩协议与HDR`这一项：
  它描述的是**向主机请求的编码协议**，不是本机显示能力。选 `2` 即同时请求 10 位档位（§3）与 `/launch` 的
  `hdrMode`、SDP 的 `dynamicRangeMode`（都取连接里 HDR 的**基座传输** `hdrMode`：`1` = HDR10/PQ、`2` = HLG），
  `colorRange` 仍是 limited（PQ/HLG 的约定）；**主机是否真在 HDR 由它自己在控制流上回**（`setHdrMode`），
  **本机显示不了就降级**（§2），两者都不成立时就是一个普通的 SDR 会话。
- `streamingRemotely` 用 `STREAM_CFG_AUTO`（由库按 RFC1918 判本地/远程，影响带宽/音频/QoS 广告与包大小）。
- `bitrate` 经 `x-ml-video.configuredBitrateKbps` 交 Sunshine 反推实际编码码率。
- 分辨率/帧率走 `clientViewportWd/Ht` 与 `maxFPS`。`maxFPS` **不是**用户填什么就发什么：连接时按
  `min(面板最高刷新率, 解码器对该尺寸上报的最高帧率)` 钳制（两者都取「平台不报即不计」），依据是帧率受
  「每秒宏块数（H.264）/ 每秒采样数（HEVC）」的**档位上限**约束——同一分辨率换编码或档位才可能过线，
  平台把该值按尺寸经 `OH_AVCapability_GetVideoFrameRateRangeForSize` 报出。钳制与提示的落点见
  [`moonlight-connection.md`](moonlight-connection.md) §3。
- `clientRefreshRateX100` 仅用于 NTSC 分数帧率：传入的客户端刷新率落在请求帧率 ±2% 内才生效，否则为 0；
  故它取**面板刷新率 × 100**、面板刷新率与最终帧率不在此范围时置 0（连接层给出，见
  [`moonlight-connection.md`](moonlight-connection.md) §3）。
- **HDR 亮度上报走 `/launch`，与 `STREAM_CONFIGURATION` 无关**：`maxBrightness` / `minBrightness` /
  `maxAverageBrightness`（基地版扩展，见 [`moonlight-control.md`](moonlight-control.md) §3）描述的是**本机显示
  能力**，主机把它写进虚拟显示器的 HDR 亮度范围（`{GUID}:[max,min,maxFALL]`，`vdd_utils::create_vdd_monitor`），
  Windows 据此把 SDR 桌面映射进 HDR 信号——**不上报就沿用主机的 1000 / 0.001 / 1000 默认值，SDR 内容会偏亮**。
  三个值的口径（`hdr::client_display_capabilities`）：`max` 是小窗口**激活峰值**、`maxAverage` 是**全屏可持续
  亮度**（ABL 面板明显低于峰值）、`min` 是黑位；主机按**一份整体**校验（`min ≤ maxAverage ≤ max`，缺一或越界
  即整份作废回默认），所以本机解析不出峰值时**一份都不发**。
  - **这不是我们能直接设的"虚拟屏 SDR 白位"**：主机侧没有任何这类设置项，驱动（ZakoVDD）只把 Windows 给出的
    `SdrWhiteLevel` 读回来转发（`IddCxMonitorQueryHardwareCursor3`，缺省 80 nit）。这三个值真正落地的是**虚拟显示器
    EDID 的 CTA HDR 静态元数据**（byte4 = max、byte5 = maxFALL、byte6 = 黑位，`EdidLuminance.h`）与
    `EvtIddCxMonitorSetDefaultHdrMetadata`；Windows 拿它当这块屏的能力。**值一旦变化超过主机容差
    （每字段 ±max(25 nit, 5%)）主机就重建虚拟显示器**，Windows 重读 EDID 并重算 SDR 白位 ⇒ 画面亮度会跳。
  - **峰值是实测推导值，两项口径不同、不能互为默认**：`display.getBrightnessInfo()` 只给 `sdrNits`
    （当前 SDR 白点绝对亮度）、`currentHeadroom`（相对它的**当前**余量倍率）与 `maxHeadroom`（最大余量倍率）
    ——都是倍率不是亮度读数；平台**不提供**黑位，也**没有** EDID 的 `desiredMaxAverageLuminance`
    （安卓版读的就是那个真实值）。故：
    `max = sdrNits × max(maxHeadroom)`（吸附标准档 `[400,600,800,1000,1200,1600,2000,2600,4000]`，偏离 >30% 保留原值），
    `maxAverage = max × 0.2`，黑位固定主机默认 `0.001`。
    **`maxAverage` 不能"跟随峰值"**：那等于宣称这块屏没有 ABL 限制，中高亮度内容会整体偏亮；
    `×0.2` 是 moonlight-vplus（口径标准）在面板不报该值时的取法，我们没有 EDID 读数，只有这一条路。
  - **`自动` 的峰值 = 连接时窗口采样**（5×250ms，≥3 个有效样本，`sdrNits ≥ 50`，样本波动比 ≤ 1.5，
    峰值 ≥ 100 且 ≥ sdrNits），不可靠就**不上报**（主机用自己的默认值 / 有校准档时用校准档）。采样与档位吸附
    是照 moonlight-harmony 的做法：安卓版的峰值直接读 EDID、天生稳定，而 `sdrNits` 随亮度条漂移，单次读数
    会越过主机的 VDD 容差而触发上面那条重建。`maxAverage` 按固定比例算，不参与采样；手动档位则直接用用户值。
    判定与换算在 `utils/MoonlightHdr.ets`；设置页另用 `brightnessInfoChange` 让**显示的**检测值跟着亮度条变，
    并**直接显示吸附后的档位值**——页面给出的就是会上报的那个数，不是原始读数（差别只是单次读数 vs 连接时的
    多采样中位）。平均亮度那行的说明也只写它是什么，不写"峰值的 20%"这类中间口径。

## 5. 与 RDP 呈现器的复用口径

- RDP 的 `FramePipeline` / `GdiFrameHost` / `PresentGdiFrame` / 零拷贝桌面缓冲都是 **gdi（CPU 帧）专属**，
  不套给 Moonlight。
- `FramePresenter`（surface / swapchain / letterbox / 超分）是唯一可复用的图形核心，**Moonlight 视频只在超分时走它**：
  接口只有一个视频入口 `PresentVideo`（帧 + 几何 → 呈现槽位 token）与 `WaitVideoFrame(token)`，且**只由
  `VkRenderer` 实现**（`GlesPresenter` 不参与，所以非超分会话「落在」surface 链路上——它们根本不建呈现器）。
  可复用的是**「会话分辨率的一张输入图 →（可选）超分 → letterbox → swapchain → present」这一层**；
  RDP 的桌面缓冲 / 脏区 / 合并（carry）是 **RDP 专属的可选路径**，视频链路一概不用——视频恒整帧一次、
  无 CPU 拷贝，送显节拍是"等槽位 + 被新帧接管"而不是合并。surface 链路**完全不经 presenter**，
  与 RDP 互不影响（见 [`present.md`](present.md) §1/§4）。

## 6. 不采用的方向

- **不采用「解码 → `OH_NativeImage` 外部纹理 → GLES presenter」这条中间方案**：外部纹理的朝向/裁剪在
  `GetTransformMatrixV2` 的矩阵里、须与纹理坐标配合，还要处理跨线程 EGL，复杂度与故障面都不划算。
  同样一趟整帧 GPU pass 由超分链路用官方 `VK_OHOS_external_memory` 导入 + 自写 convert 完成（§2），
  不必再引入外部纹理这一层；GLES 呈现器只服务 RDP 的兜底上屏，不接视频。
- **YUV 4:4:4 不做**：解码与上屏按 4:2:0 处理。
