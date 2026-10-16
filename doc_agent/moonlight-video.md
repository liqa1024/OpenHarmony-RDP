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
  不要拿窗口/surface 尺寸当提示（会为远小于它的码流过度分配），会话侧按协商的流分辨率设。buffer 模式另设
  `AV_PIXEL_FORMAT_NV12`。**实际可见尺寸**由 `onStreamChanged` 的 `OH_MD_KEY_VIDEO_PIC_WIDTH/HEIGHT` 给出，
  只作解码器的可见尺寸参考；它**不**改组件几何（组件比例取请求的流分辨率，见 §2）。
- **帧率进 Configure，不置可变帧率开关**：`OH_MD_KEY_FRAME_RATE` 取协商帧率（键声明为 double、示例传
  int，故按 setter 的类型校验择一设置）；越界会让 Configure 回 `AV_ERR_INVALID_VAL`，故失败时**不带帧率
  重试一次**。`OH_MD_KEY_VIDEO_DECODER_OUTPUT_ENABLE_VRR` 不置：这里**不需要**它——显示端吃不下由上屏闸门
  自己丢，平台那套刷新率自适应不是链路的必要条件；协议侧也没有这一项（Moonlight 按协商 `maxFPS` 送帧，
  没有刷新率握手），故它不构成一个可加的开关。
- **上屏闸门（静默丢弃）**：表面直出只送显示端吃得下的帧——按面板当前刷新率留送显槽位，落在槽位前半个
  周期以上的帧**直接释放**（已解出的帧不影响预测链，故不请关键帧、不报错）。没这道闸门时队列写满会让
  送显阻塞、输出缓冲不再回来，整条解码链停摆。刷新率取 `OH_NativeDisplayManager_GetDefaultDisplayRefreshRate`
  并跟随显示变化监听，读不到就不设闸门。**闸门只丢不等**（不 sleep、不建时间轴、不持有帧）：实时链路里"等"
  就是延迟堆积，故参考官方 Surface 播放控制时只取"太晚就丢"那一半，判据用面板刷新率代替音频时钟。
- **硬解优先，软解兜底，无解器报错**：`VideoDecoderSupport()`（`OH_AVCodec_GetCapabilityByCategory`）把每种
  编码分成**硬解 / 软解 / 无**，优先创建硬解，硬解缺失时才退到软解，`VideoDecoder::hardware()` 反映实际
  选择；完全没有解码器时起播同步失败并给出原因，绝不静默黑屏。
- **送显即出帧，客户端不排程**：surface 模式解码出帧就送显（吃不下由上一条的闸门丢），节拍由显示器 vsync
  与主机按协商 `maxFPS` 的发送共同决定。客户端自建时间轴（帧计数配协商帧率 + 首帧墙钟推
  `RenderOutputBufferAtTime`）不采用：与码流时间无关，主机实际帧率不等于协商值时偏差只累加、不回正，表现为
  静默的延迟堆积；且 SDK 约定输出 buffer **顺序处理**，被按住的 buffer 会阻塞后续。
- **输入按延迟预算排队，超预算丢积压并请主机补关键帧**：解码桥不把提交透传给 codec，而是入一个按**时间**
  （不是字节）计量的队列——预算 = 3 个帧周期、钳在 30–150ms，帧率未知时 75ms。队列里最老的一帧超过预算即
  判定「已经太晚、显示无意义」，于是**丢掉整段积压**并回 `DR_NEED_IDR`：库收到该返回值后清掉自己的分帧状态、
  停止送帧直到新 IDR 到达，所以一次过载只花一个关键帧就回到基线延迟，不会形成关键帧风暴（限流由库侧完成）。
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
- **解码器一处两类输出**（`VideoOutputMode`）：surface 给直出，buffer 给超分。喂入、回调派发、生命周期、
  背压队列共享，差异只在输出回调与是否 `SetSurface`——不是两份解码逻辑。

## 2. 上屏：两条链路

Moonlight 的视频有两条**互相独立**的链路，按是否需要超分选择——是显式模式，不是"先试一条再回退另一条"。

- **默认：解码直出 XComponent**。surface 模式（`OH_VideoDecoder_SetSurface(codec, window)`，必须在
  `Prepare` 之前）把解码帧直接写进会话页 XComponent 的 `OHNativeWindow`。应用侧**没有呈现器**、没有 GPU
  合成 pass；留边由**画面组件几何**承担——会话页用 `components/SessionPicture` 把 surface 约束成**流分辨率
  比例并居中**（与 RDP 同一套 `utils/PictureFit`），组件比例恒等于内容比例，合成器在窗口拖拽时只做等比缩放，
  **无需重绘**。窗口另设 `OH_NativeWindow_NativeWindowSetScalingModeV2(window, OH_SCALING_MODE_SCALE_FIT_V2)`
  （不设则默认拉伸填满），兜住协商流分辨率与请求值不一致的情形。留边区域**不接收指针**：输入/焦点在 XComponent
  上，事件坐标即画面局部坐标，映射是整画面归一化。这是平台文档推荐的视频播放方式，延迟/功耗最优，全屏原生
  比例时可能命中硬件直通/overlay。
- **超分：buffer 模式 + Vulkan 导入，复用 RDP 呈现器**。`OH_AVBuffer`（底层 `OH_NativeBuffer`，NV12）经
  `VK_OHOS_external_memory`（`vulkan_ohos.h`）导入为 `VkImage`，YUV→RGB 在 shader 里做，之后接
  `VkRenderer` 既有的 letterbox / 超分 / swapchain。此链路**只在真机验证**（Vulkan 呈现本就真机）。

## 3. 编解码协商

支持的编码是 H.264 / HEVC / AV1（`video/avc` / `video/hevc` / `video/av1`）。
协商**由客户端单向决定、主机只广告不自选**：主机在 RTSP `DESCRIBE` 的 SDP 里列出**能编码的全部格式**
（Sunshine 用 `sprop-parameter-sets=AAAAAU` 表示 HEVC、`AV1/90000` 表示 AV1），客户端库在本机
`supportedVideoFormats` 与该广告的交集上按固定优先级 `AV1 > HEVC > H.264` 取一，再用 `ANNOUNCE` 的
`x-nv-vqos[0].bitStreamFormat` 回报。**决策权不交给库**：`moonlightVideoFormats(choice, serverCodecModeSupport)`
直接产出**恰好一个** `VIDEO_FORMAT_*`（非能力集合），库只负责照此回报：

- **自动**（`ML_CODEC_AUTO`）：硬件档 `AV1→HEVC→H264`、无硬解再软件档同序逐级下退，取第一个「本机该档可解
  且主机可编码」的。H.264 不查主机（库的兜底就是它）；AV1 / HEVC 要求 `SCM_MASK_AV1` / `SCM_MASK_HEVC`——
  Sunshine 的 `SCM` 位与 `DESCRIBE` 广告同源（`active_*_mode >= 2` ⟹ `!= 1`），故选中者必被库确认。
- **手动**（连接编辑页 / 设置页的编码选择，默认自动、可会话独立覆盖）：**强制**所选编码，同样要求本机可解
  且主机可编码。选择项按 `moonlightVideoCodecSupport()`（硬解 / 软解 / 无）置灰，仅软解时下方红字提示无硬件
  解码支持，无解码器时提示无法串流。
- **无共同编码即显式失败**：`moonlightVideoFormats` 返回 `0` 时 ArkTS 在 `LiStartConnection` 前直接失败；
  会话中若主机协商出非决策编码，`setup` 拒绝（`codec-mismatch`），不静默改推别的编码。

协商结果只有软解时以 `software-decode|<mime>` 警告（提示主机换编码器）。`moonlightVideoCodec()` 给出编码
标记（h264/hevc/av1），工具栏把它写进分辨率括号（`1920 x 1080 (HEVC)`；软解再加 CPU，如 `(HEVC, CPU)`，
见 [`moonlight-session.md`](moonlight-session.md) §2）。

## 4. 其余 `STREAM_CONFIGURATION` 字段的落点

- `colorSpace` / `colorRange` 合成 SDP `x-nv-video[0].encoderCscMode`（`(space<<1)|range`）交主机做编码 CSC——
  SDR 取 Rec709 + limited，`0` 是 Rec601（HD 下偏色）。
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

## 5. 与 RDP 呈现器的复用口径

- RDP 的 `FramePipeline` / `GdiFrameHost` / `PresentGdiFrame` / 零拷贝桌面缓冲都是 **gdi（CPU 帧）专属**，
  不套给 Moonlight。
- `FramePresenter`（surface / swapchain / letterbox / 超分）是唯一可复用的图形核心，**只被超分链路复用**：
  以 buffer-only 形式接入视频输入，且**只由 `VkRenderer` 实现**（`GlesPresenter` 不参与）。直出链路
  **完全不经 presenter**，与 RDP 互不影响（见 [`rdp-present.md`](rdp-present.md)）。

## 6. 不采用的方向

- **不采用「解码 → `OH_NativeImage` 外部纹理 → GLES presenter」这条中间方案**：对全屏视频它比直出多一个
  整帧 GPU pass（letterbox / YUV→RGB），换来的是一条并非必需的 present 通道；且外部纹理的朝向/裁剪在
  `GetTransformMatrixV2` 的矩阵里、须与纹理坐标配合，还要处理跨线程 EGL，复杂度与故障面都不划算。
  Moonlight 的 letterbox 由直出窗口的 `SCALE_FIT` 缩放模式解决，超分则走 Vulkan 导入链路。
- **YUV 4:4:4 不做**：解码与上屏按 4:2:0 处理。
