# Moonlight 原生客户端

本文件是 **Moonlight / Sunshine 串流的原生侧**知识：客户端库的拉取/补丁/构建与静态链接、视频解码与上屏、
桥接层约定，以及后续的串流会话与输入接线。**控制层（serverinfo / pair / applist / launch）、协议编排与全部
XML 解析在 ArkTS**（§4），不在本文件范围。**前端**侧（协议建模、存储布局、连接列表与
编辑页、设置页分栏、图标）见 [`moonlight-connection.md`](moonlight-connection.md)；RDP 的
FreeRDP/OpenSSL/zlib 构建见 [`native-libraries.md`](native-libraries.md)（本文件不重复它）。

## 1. 客户端库（moonlight-common-c）的构建与链接

- **客户端库选型**：`moonlight-common-c` 是各 Moonlight 客户端共用的核心，但**本仓库固定的这一版只做 RTSP
  会话协商与 control/audio/video 流的分帧**：**HTTP 层（`/serverinfo`、`/pair`、`/applist`、`/launch`
  `/resume`）不在库里**，由客户端实现后把结果填进 `SERVER_INFORMATION`（`serverCodecModeSupport` /
  `serverInfoAppVersion` / `rtspSessionUrl`）与 `STREAM_CONFIGURATION`（含 `remoteInputAesKey/Iv`）交
  `LiStartConnection`。上游**没有 release tag**，按 commit 固定。
- **依赖树**：它的 `enet` 与 `nanors` 是 **git submodule**，commit tarball 不含；`fetch-sources.ps1` 分别按
  commit 固定拉取后放进 `moonlight-common-c/{enet,nanors}`（它的 CMake 用 `add_subdirectory(enet)` 并在
  `nanors/rs.c` 处编译）。enet 上游恒为静态库。
- **构建**：随 `native/scripts/build-native.ps1` 的 moonlight 步一起（Windows NDK 交叉编译，静态归档）。
  `fetch-sources.ps1` 负责按 commit 拉取并自动跑 `patch-moonlight.ps1`。

- **静态链接，不单独打包**：与 FreeRDP 不同，它没有运行时加载插件、不需要导出符号，因此构建成静态库
  （`-DBUILD_SHARED_LIBS=OFF`），连同 enet、OpenSSL 的 `libcrypto.a` 一起链进 `libhmrdp.so`；
  `entry/libs/<abi>/` 里**不新增 `.so`**。两者都带 `-fPIC`。
- **归档与头文件位置**：上游 CMakeLists 不写 install 规则，构建步手工把 `libmoonlight-common-c.a` /
  `libenet.a` 与公开头 `Limelight.h` 收集到 `native/install/<arch>/moonlight/`，再同步到
  `entry/src/main/cpp/thirdparty/moonlight/{include,lib/<abi>}/`（生成目录，**不入库**，与 FreeRDP 头文件同策）。
  同步按 arch 只替换 `lib/<abi>`，两个 ABI 可共存。
- **OpenSSL 依赖**：库自身用 OpenSSL 做 control stream 的 AES/GCM + 随机数（`PlatformCrypto.c`）；构建时
  显式传 `OPENSSL_ROOT_DIR` / `OPENSSL_CRYPTO_LIBRARY` 等，使 `find_package(OpenSSL 1.0.2 REQUIRED)`
  命中 OHOS 版本而不是宿主（宿主没有 OHOS 版）。
- **平台层**：它自带 POSIX 平台实现（`Platform*.c`，`LC_POSIX` 分支），OHOS 直接可用；配置阶段的自检
  （`fcntl` / `ioctl` / `poll` / `getaddrinfo` / `gethostbyname_r` / `inet_pton` / `clock_gettime` 的
  `CLOCK_MONOTONIC`）全部命中。
- **补丁**：`patch-moonlight.ps1` 去掉上游在非 MSVC/MinGW 平台无条件加的 `-Werror`（它加在 `CMAKE_C_FLAGS`
  之后，命令行关不掉）。warnings 仍可见，只是不再中断构建。

## 2. 视频解码与上屏（双链路）

Moonlight 的视频有两条**互相独立**的链路，按是否需要超分选择——是显式模式，不是"先试一条再回退另一条"。
解码用 HarmonyOS AVCodec（`OH_VideoDecoder_*`，库 `libnative_media_vdec.so` / `libnative_media_core.so` /
`libnative_media_codecbase.so`）；`hmrdp_video_decoder` 一个实例包一个硬件解码器，输入是 **Annex-B 字节流**；**一次 Submit 必须是一个完整的访问单元
（一帧的全部 slice + 其前置 SPS/PPS/SEI）**——AVCodec 的硬性约束是「同一帧的多个 slice 需一次送入解码器」，
把一帧拆到多次 Push 会被当作损坏输入，解出来的画面即花屏/错乱。Moonlight 的拆包器本就把一帧的所有
NAL 交给一次回调（见 §3 的解码桥），所以产品链路天生满足这个约束。`video/avc` = H.264，`video/hevc` = HEVC。

- **默认：解码直出 XComponent**。surface 模式（`OH_VideoDecoder_SetSurface(codec, window)`，必须在
  `Prepare` 之前）把解码帧直接写进会话页 XComponent 的 `OHNativeWindow`。应用侧**没有呈现器**、没有 GPU
  合成 pass；letterbox 由组件宽高比承担（非原生比例时留边）。这是平台文档推荐的视频播放方式，延迟/功耗
  最优，全屏原生比例时可能命中硬件直通/overlay。
- **超分：buffer 模式 + Vulkan 导入，复用 RDP 呈现器**。`OH_AVBuffer`（底层 `OH_NativeBuffer`，NV12）经
  `VK_OHOS_external_memory`（`vulkan_ohos.h`）导入为 `VkImage`，YUV→RGB 在 shader 里做，之后接
  `VkRenderer` 既有的 letterbox / 超分 / swapchain。此链路**只在真机验证**（Vulkan 呈现本就真机），是
  **独立后续里程碑**。
- **解码器一处两类输出**（`VideoOutputMode`）：surface 给直出，buffer 给超分。喂入、回调派发、生命周期、
  背压队列共享，差异只在输出回调与是否 `SetSurface`——不是两份解码逻辑。
- **送显与控速**：surface 模式每帧 `RenderOutputBuffer`（丢弃用 `FreeOutputBuffer`）；要控速/音画同步用
  `RenderOutputBufferAtTime`（时间戳取自 `steady_clock`），由解码器控制节奏，应用侧不阻塞在呈现上。
- **低时延输出 = 解码序，故码流不得含 B 帧**：Configure 使能 `OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY` 后解码器
  **按解码序输出、不做重排**（交互场景要的正是这个低延迟）。送显时间轴因此就是解码序，只有码流**不含 B 帧**
  时解码序才等于显示序。Moonlight / Sunshine 的编码本就是低延迟无 B 帧，产品链路成立；换成带 B 帧的码流
  会以错误时序上屏（看着像跳帧、帧计数虚高），那是输入不符合该前提，不是解码缺陷。
- **解码器配置口径**：Configure 的宽高是**解码器实例的最大分辨率**（SDK 据此分配资源），**不是**上屏尺寸：
  不要拿窗口/surface 尺寸当提示（会为远小于它的码流过度分配），会话侧按协商的流分辨率设。buffer 模式另设
  `AV_PIXEL_FORMAT_NV12`。**实际可见尺寸**由 `onStreamChanged` 的 `OH_MD_KEY_VIDEO_PIC_WIDTH/HEIGHT` 给出，
  直出按它设组件宽高比。
- **硬解优先，软解兜底，无解器报错**：`VideoDecoderSupport()`（`OH_AVCodec_GetCapabilityByCategory`）把每种
  编码分成**硬解 / 软解 / 无**，优先创建硬解，硬解缺失时才退到软解，`VideoDecoder::hardware()` 反映实际
  选择；完全没有解码器时起播同步失败并给出原因，绝不静默黑屏。
- **编码协商（自动 + 手动偏好）**：支持的编码是 H.264 / HEVC / AV1（`video/avc` / `video/hevc` / `video/av1`）。
  `MoonlightVideoFormats(preference)` 产出交给主机的 `supportedVideoFormats` 位图：
  - **自动**（`ML_CODEC_AUTO`）：声明本机**所有能硬解的编码**（HEVC / AV1 / H.264），由主机按自身偏好
    （HEVC / AV1 优先于 H.264）选；只要存在任一硬解就不声明只能软解的编码，整机无硬解时才声明软解集合。
  - **手动**（连接编辑页 / 设置页的编码选择，默认自动、可会话独立覆盖）：只声明所选编码；该编码本机不能解
    时回落自动。选择项按 `moonlightVideoCodecSupport()`（硬解 / 软解 / 无）置灰不可选，仅软解时该组下方给
    红色小字提示无硬件解码支持，无任何解码器时提示无法串流。
  协商结果只有软解时以 `software-decode|<mime>` 警告（提示主机换编码器）。`MoonlightVideoCodec()` 给出编码
  标记（h264/hevc/av1），工具栏把它写进分辨率括号（`1920 x 1080 (HEVC)`；软解再加 CPU，如 `(HEVC, CPU)`）。

### 与 RDP 呈现器的复用口径

- RDP 的 `FramePipeline` / `GdiFrameHost` / `PresentGdiFrame` / 零拷贝桌面缓冲都是 **gdi（CPU 帧）专属**，
  不套给 Moonlight。
- `FramePresenter`（surface / swapchain / letterbox / 超分）是唯一可复用的图形核心，**只被超分链路复用**：
  届时以 buffer-only 形式加回视频输入，且**只由 `VkRenderer` 实现**（`GlesPresenter` 不参与）。直出链路
  **完全不经 presenter**，与 RDP 互不影响。

### 不采用的方向

- **不采用「解码 → `OH_NativeImage` 外部纹理 → GLES presenter」这条中间方案**：对全屏视频它比直出多一个
  整帧 GPU pass（letterbox / YUV→RGB），换来的是一条并非必需的 present 通道；且外部纹理的朝向/裁剪在
  `GetTransformMatrixV2` 的矩阵里、须与纹理坐标配合，还要处理跨线程 EGL，复杂度与故障面都不划算。
  Moonlight 的 letterbox 由直出的组件宽高比解决，超分则走 Vulkan 导入链路。

## 3. 桥接层与接线

- **构建必备输入，不是平台能力**：`moonlight-common-c` 是**我们自己 vendor 的软件依赖**（随
  `build-native.ps1` 按固定版本编出、链进 `libhmrdp.so`），和 FreeRDP 同类，**不是**设备/系统可能缺失的
  平台能力。所以 `CMakeLists.txt` 检查归档（`libmoonlight-common-c.a` / `libenet.a`）与 `libcrypto.a` 是否
  齐全，缺一即 `FATAL_ERROR` 并提示先跑 `build-native.ps1`，随后**无条件**链接、无条件定义头文件路径。
  **不**照搬 FSR / XEngine「缺依赖就编成不支持、构建照常」的降级写法——那是"设备能力有无"的模型，套到自研
  功能上会造出一个本不该存在的"无 Moonlight"产品态。
- **静态库必须真被引用才算连上**：静态归档里没被引用的目标不会进 `.so`。`hmrdp_moonlight.cpp` 的探针
  真正调用一个 moonlight 入口（`LiInitializeStreamConfiguration`，纯字段初始化、不起线程/不建 socket），
  所以"符号已解析"由**链接器**保证：有未解析符号则模块链接失败。
- **N-API 探针**：`moonlightProbe` 恒回 `"1"`——它只是一个"归档确实解析进了模块"的链接断言，不是能力探测
  （不用于 UI 置灰，也不该有"本设备不支持 Moonlight"这种状态）。
- **解码桥**：`hmrdp_moonlight.cpp` 把 AVCodec 解码器接进 moonlight-common-c 的解码渲染回调
  （`DECODER_RENDERER_CALLBACKS`，见 `hmrdp_moonlight.h`）。`MoonlightVideoCallbacks()` 交回一张静态
  回调表供 `LiStartConnection` 使用；`MoonlightVideoContext`（经 `renderContext` 传入）带会话的
  `OHNativeWindow`。
  - `setup` 把 `VIDEO_FORMAT_*` 映射成 MIME，并按 §2 的能力模型选定解码路径。
  - `submitDecodeUnit` 把该回调交来的 **Annex-B 缓冲链**（一帧的全部 NAL，正好等于一个访问单元）拉平后
    交给 `VideoDecoder::Submit`，即 §2 的「一次一个完整访问单元」。
  - `start`/`stop`/`cleanup` 驱动解码器生命周期。能力位留空：不改写参考帧，且**不开
    `CAPABILITY_DIRECT_SUBMIT`**，让提交跑在 moonlight 自己的解码线程上而不是 RTP 接收线程。
  - 输入积压超过阈值时回 `DR_NEED_IDR` 请主机补关键帧，避免无界排队。
- **串流会话桥**：`core/hmrdp_moonlight_session.*` 包住 `LiStartConnection`。**`LiStartConnection` 在建流
  完成后就返回**，会话此后跑在 moonlight 自己的线程上，直到 `LiStopConnection`——所以两者必须**在同一条会话
  线程上成对调用**：会话线程执行 `LiStartConnection` → 等停止/被终止的通知 → 执行 `LiStopConnection`；
  调用方（UI 线程）只置停止标志、发条件变量通知、`join`，绝不直接碰 moonlight 状态。`g_active` 覆盖整个
  会话生命周期（含已建流阶段），用来**挡住并发的第二个会话**（moonlight-common-c 是进程级单连接，重入会把
  它的全局状态搞坏，表现为 `VideoRecv`/`RtpvAddPacket` 崩溃）。视频回调取自解码桥，`renderContext` 是会话
  XComponent 的 `OHNativeWindow`，音频暂以空渲染器关掉（Opus 渲染器未定，见
  [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)）；输入直接转 `LiSend*`（相对/绝对鼠标、按键、滚轮）。
  N-API 出口：`moonlightSetSessionListener`（进程级事件回调 `(event, value, text)`）、
  `moonlightSessionSetSurface/ClearSurface/Start/Stop/Active`、`moonlightMouse*` / `moonlightKey` /
  `moonlightScroll`；窗口是**进程级单个** `OHNativeWindow`，须在 `Start` 前绑定、`Stop` 后释放
  （`LiStartConnection` 的 `renderContext` 需要它）。

## 4. 控制 / 配对层（ArkTS）

控制层（`/serverinfo`、`/pair`、`/applist`、`/launch` / `/resume`）、配对编排与全部 XML 解析都在 **ArkTS**
（见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)）：传输与 mTLS / 证书 pin 用 `@ohos.net.http`，配对加密
原语用 `cryptoFramework` 与 `@ohos.security.cert`，客户端身份（自签名 X.509 + HUKS 信封加密私钥）在 ArkTS
组装；**原生不含这一层**。落地在 `ets/moonlight/`：`MoonlightCrypto`（配对加密原语）、`MoonlightIdentity`
（身份与 pin 存储）、`MoonlightHttp`（传输）、`MoonlightClient`（编排），串流前预检由
`services/MoonlightConnector` 调用（`serverinfo → pair → launch`，PIN 由调用方弹窗提供），协议建模与响应解析见
[`moonlight-connection.md`](moonlight-connection.md)。以下是与实现语言无关的协议事实：

- **信任模型 = 证书 pin**：未配对时接受任意主机证书（首触）；配对后存叶证书，之后逐字节一致。主机自签名，
  信任靠 pin 而非 CA。
- **端口**：serverinfo 与配对前三步走明文 **HTTP 47989**，`pairchallenge` 走 **HTTPS 47984**（mTLS）。
- **配对时序**：客户端先发起 `getservercert`——**主机正是收到它才挂起并提示输入 PIN**，所以客户端要
  **先发请求、再显示 PIN**（两者并行，不是"先等用户输入再发"）；随后
  `clientchallenge → serverchallengeresp → clientpairingsecret → pairchallenge`。
  `getservercert` 要等主机端输入完成，**无读超时**（其余 5s 量级）。同一轮配对的
  `uniqueid` 必须贯穿五步一致；本实现**每轮配对换一个新 `uniqueid`**——主机按它给挂起会话建索引
  （Sunshine `map_id_sess`），复用旧 id 会得到 409 且不再弹 PIN。
- **客户端身份**：自签名 X.509，RSA-2048、`SHA256withRSA`、20 年、`CN=NVIDIA GameStream Client`；证书 PEM 用
  LF 换行（CRLF 会被主机拒收）。**上线的证书是 PEM 文本的 hex**（`clientcert` 参数、以及主机返回的
  `plaincert`），主机按 PEM 解析（Sunshine `crypto::x509` 走 `PEM_read_bio_X509`）——把 DER 的 hex 当作
  参数会配对失败。私钥落盘用 HUKS 信封加密（不可导出的 AES-256-GCM 密钥），证书明文落盘；
  不用 `certManager`/ASSET：前者私钥不可导出、无法用于 mTLS 握手，后者容量 ≤ 1KB 放不下 PKCS8 私钥。
- **配对原语**（hex 进 hex 出）：`aesKey = SHA256(salt || pin)[0:16]`——**恒 SHA-256**（Sunshine 与
  GFE 7+ 都如此；不要按版本回退 SHA-1）。AES-128-ECB、零填充、无 IV；哈希里串入的是**证书签名值字节**
  （ASN.1 BIT STRING 内容），不是公钥。
- **存储**：目录 `<dir>/moonlight/`（`dir` 为应用沙箱 files dir）；pin 按 host 存 `server_<key>.der`。
- **控制端点参数名（Sunshine）**：`/launch` 必填 `rikey` / `rikeyid` / `localAudioPlayMode` / `appid`，
  分辨率用 `mode=WxHxFPS`；HDR 是 `hdrMode`、环绕是 `surroundAudioInfo`（不是 `hdr`/`surroundaudio`），
  手柄掩码是 `gcmap`。**没有 `appid=0` 的"桌面"**：未指定应用时先取 `/applist` 选一个（优先名为 Desktop 的）。
- **主机被占用**：`/launch` 会回 400 `An app is already running on this host`；客户端可 `GET /cancel`
  （HTTPS + mTLS）结束在跑的应用后再试，`/resume` 则复用既有会话。
- **明文 `serverinfo` 恒 `PairStatus=0`**：Sunshine 只在 HTTPS 请求里按 `uniqueid` 置 1，所以"是否已配对"
  以本地存的 pin 为准（有 pin 即跳过配对），不能只信明文 serverinfo。

## 5. 范围与后续方向

- 本文件覆盖：客户端库的构建与链接（§1）、视频解码与协商（§2）、桥接层与接线（§3，解码桥 + 串流会话桥）、
  控制/配对层的协议事实（§4）。
- 超分链路（buffer 模式 + Vulkan 导入 + 复用 `VkRenderer`）只在真机验证，属独立后续里程碑，复用约束见 §2
  「与 RDP 呈现器的复用口径」。
- 主机发现、应用列表交互不在本文件范围；**控制层（驱动 `serverinfo → pair → applist → launch`）、协议编排与
  XML 解析在 ArkTS**（§4），会话的 ArkTS 接线（N-API / `MoonlightSessionBackend` / 会话窗口）与输入页路由
  也不在本文件范围。路线与阶段见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)。
- **改前必读**：会话侧接线要遵守既有的线程模型（动态通道分发线程只做轻活，帧重活走 sink 钩子），见
  [`architecture.md`](architecture.md) §4。
