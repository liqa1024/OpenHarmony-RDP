# Moonlight 原生客户端

本文件是 **Moonlight / Sunshine 串流的原生侧**知识：客户端库的拉取/补丁/构建与共享库链接、桥接层与串流会话、
以及三个原生扩展（本地光标模式、原生触控板、音频）。**视频解码、上屏与编解码协商**见
[`moonlight-video.md`](moonlight-video.md)；**控制 / 配对层（ArkTS）**见 [`moonlight-control.md`](moonlight-control.md)；
**前端**（协议建模、存储、连接列表与编辑页、设置页分栏、图标）见 [`moonlight-connection.md`](moonlight-connection.md)，
**主机与应用**见 [`moonlight-apps.md`](moonlight-apps.md)，**会话窗口与遥测**见 [`moonlight-session.md`](moonlight-session.md)；
RDP 的 FreeRDP/OpenSSL/zlib 构建体系见 [`native-build.md`](native-build.md)（本文件不重复它）。

## 1. 客户端库（moonlight-common-c）的构建与链接

- **客户端库选型**：`moonlight-common-c` 是各 Moonlight 客户端共用的核心，但**本仓库固定的这一版只做 RTSP
  会话协商与 control/audio/video 流的分帧**：**HTTP 层（`/serverinfo`、`/pair`、`/applist`、`/launch`
  `/resume`）不在库里**，由客户端实现后把结果填进 `SERVER_INFORMATION`（`serverCodecModeSupport` /
  `serverInfoAppVersion` / `rtspSessionUrl`）与 `STREAM_CONFIGURATION`（含 `remoteInputAesKey/Iv`）交
  `LiStartConnection`。上游**没有 release tag**，按 commit 固定。
- **依赖树**：它的 `enet` 与 `nanors` 是 **git submodule**，commit tarball 不含；`fetch-sources.ps1` 分别按
  commit 固定拉取后放进 `moonlight-common-c/{enet,nanors}`（它的 CMake 用 `add_subdirectory(enet)` 并在
  `nanors/rs.c` 处编译）。
- **构建**：随 `native/scripts/build-native.ps1` 的 moonlight 步一起（Windows NDK 交叉编译）。
  `fetch-sources.ps1` 负责按 commit 拉取并自动跑 `patch-moonlight.ps1`。
- **共享库，单独打包**：上游 CMakeLists **默认就是共享**（`BUILD_SHARED_LIBS` 未定义时自行置 ON），
  `build-native.ps1` 不覆盖它 ⇒ 产出 `libmoonlight-common-c.so`，与其它独立依赖一样拷进
  `entry/libs/<abi>/`，`libhmrdp.so` 按完整路径链它。
  **`enet`（以及 `nanors`）不单独成库**：它们是本库的 submodule、属实现细节而非独立依赖，
  `enet/CMakeLists.txt` 把 `add_library(enet STATIC)` 写死（命令行上的 `BUILD_SHARED_LIBS` 也到不了
  `add_subdirectory(enet)`），于是照旧静态编进 `libmoonlight-common-c.so`——判据见
  [`native-build.md`](native-build.md) §1。
- **头文件位置**：上游 CMakeLists 不写 install 规则，构建步手工把公开头 `Limelight.h` 收集到
  `native/install/<arch>/moonlight/`，再同步到 `entry/src/main/cpp/thirdparty/moonlight/include/`
  （生成目录，**不入库**，与 FreeRDP 头文件同策）；`.so` 走统一的 `entry/libs/<abi>/` 出口，按 arch 各自一份。
- **OpenSSL 依赖**：库自身用 OpenSSL 做 control stream 的 AES/GCM + 随机数（`PlatformCrypto.c`）；构建时
  显式传 `OPENSSL_ROOT_DIR` / `OPENSSL_CRYPTO_LIBRARY` / `OPENSSL_SSL_LIBRARY` 并置
  `OPENSSL_USE_STATIC_LIBS=OFF`，使 `find_package(OpenSSL 1.0.2 REQUIRED)` 命中 OHOS 的 `libcrypto.so`
  而不是宿主（宿主没有 OHOS 版）。它与 FreeRDP 是 OpenSSL 的两个独立消费者，所以 OpenSSL 只以共享库存在
  一份（见 [`native-build.md`](native-build.md) §1）。
- **平台层**：它自带 POSIX 平台实现（`Platform*.c`，`LC_POSIX` 分支），OHOS 直接可用；配置阶段的自检
  （`fcntl` / `ioctl` / `poll` / `getaddrinfo` / `gethostbyname_r` / `inet_pton` / `clock_gettime` 的
  `CLOCK_MONOTONIC`）全部命中。
- **补丁**（`patch-moonlight.ps1`，一步，幂等）：去掉上游在非 MSVC/MinGW 平台无条件加的 `-Werror`
  （它加在 `CMAKE_C_FLAGS` 之后，命令行关不掉；warnings 仍可见，只是不中断构建）。

## 2. 桥接层与接线

- **构建必备输入，不是平台能力**：`moonlight-common-c` 是**本工程 vendor 的软件依赖**（随
  `build-native.ps1` 按固定版本编出、作为共享库链进 `libhmrdp.so`），和 FreeRDP 同类，**不是**设备/系统
  可能缺失的平台能力。所以 `CMakeLists.txt` 逐个检查 `libmoonlight-common-c.so` / `libcrypto.so` /
  `libssl.so`（连同 FreeRDP 与 zlib，共一份清单）是否都在 `entry/libs/<abi>/`，缺一即 `FATAL_ERROR` 并提示
  先跑 `build-native.ps1`，随后**无条件**链接、无条件定义头文件路径。
  **不**照搬 FSR / XEngine「缺依赖就编成不支持、构建照常」的降级写法——那是"设备能力有无"的模型，套到自研
  功能上会造出一个本不该存在的"无 Moonlight"产品态。
- **"符号已解析"由链接器保证**：`libhmrdp.so` 链 `libmoonlight-common-c.so`，且链接带 `-Wl,--no-undefined`
  （OHOS 工具链默认），有未解析符号则链接失败。`hmrdp_moonlight.cpp` 的探针仍真正调用一个 moonlight 入口
  （`LiInitializeStreamConfiguration`，纯字段初始化、不起线程/不建 socket），把这条约束钉在调用点上。
- **N-API 探针**：`moonlightProbe` 恒回 `"1"`——它只是一个"库确实链上了"的链接断言，不是能力探测
  （不用于 UI 置灰，也不该有"本设备不支持 Moonlight"这种状态）。
- **单会话约束**：moonlight-common-c 是**进程级单连接**，重入会把它的全局状态搞坏（表现为 `VideoRecv`/`RtpvAddPacket`
  崩溃）；`g_active` 覆盖整个会话生命周期来挡住并发的第二个会话（见下）。
- **解码桥**：`hmrdp_moonlight.cpp` 把 AVCodec 解码器接进 moonlight-common-c 的解码渲染回调
  （`DECODER_RENDERER_CALLBACKS`，见 `hmrdp_moonlight.h`）。`MoonlightVideoCallbacks()` 交回一张静态
  回调表供 `LiStartConnection` 使用；`MoonlightVideoContext`（经 `renderContext` 传入）带会话的
  `OHNativeWindow`。
  - `setup` 把 `VIDEO_FORMAT_*` 映射成 MIME，并按 [`moonlight-video.md`](moonlight-video.md) §1 的能力模型
    选定解码路径。
  - `submitDecodeUnit` 把该回调交来的 **Annex-B 缓冲链**（一帧的全部 NAL，正好等于一个访问单元）拉平后
    交给 `VideoDecoder::Submit`，即 [`moonlight-video.md`](moonlight-video.md) §1 的「一次一个完整访问单元」。
  - `start`/`stop`/`cleanup` 驱动解码器生命周期。能力位留空：不改写参考帧，且**不开
    `CAPABILITY_DIRECT_SUBMIT`**，让提交跑在 moonlight 自己的解码线程上而不是 RTP 接收线程。
  - 输入积压超过阈值时回 `DR_NEED_IDR` 请主机补关键帧，避免无界排队。
- **串流会话桥**：`core/hmrdp_moonlight_session.*` 包住 `LiStartConnection`。**`LiStartConnection` 在建流
  完成后就返回**，会话此后跑在 moonlight 自己的线程上，直到 `LiStopConnection`——所以两者必须**在同一条会话
  线程上成对调用**：会话线程执行 `LiStartConnection` → 等停止/被终止的通知 → 执行 `LiStopConnection`；
  调用方（UI 线程）只置停止标志、发条件变量通知、`join`，绝不直接碰 moonlight 状态。`g_active` 覆盖整个
  会话生命周期（含已建流阶段），用来**挡住并发的第二个会话**（见上）。视频回调取自解码桥，`renderContext`
  是会话 XComponent 的 `OHNativeWindow`，音频回调取自音频桥（见 §2.3；`enableAudio` 关闭时为空渲染器，静音）；
  输入由 `MoonlightInputSink` 落 `LiSend*`
  （相对/绝对鼠标、按键、UTF-8 文本、高分辨率滚轮、触屏触点、原生触控板接入点），策略在共用
  `SessionInput`（见 [`session-layer.md`](session-layer.md) §8）。N-API 出口：
  `moonlightSetSessionListener`（进程级事件回调 `(event, value, text)`）、
  `moonlightSessionSetSurface/ClearSurface/Start/Stop/Active`、`moonlightMouse*` / `moonlightKey` /
  `moonlightScroll` / `moonlightHighResScroll` / `moonlightHighResHScroll` / `moonlightTouch` /
  `moonlightTouchpad` / `moonlightText` / `moonlightHostFeatures` / `moonlightTelemetry` /
  `moonlightSetShortcutPassthrough`；
  窗口是**进程级单个** `OHNativeWindow`，须在 `Start` 前绑定、`Stop` 后释放
  （`LiStartConnection` 的 `renderContext` 需要它）。
- **遥测采样**：解码桥按帧累加**累计计数**（解码单元数、编码载荷字节 Σ`fullLength`、主机帧处理耗时之和 / 帧数、
  本机解码往返之和 / 帧数——最后一项在 `submitDecodeUnit` 记提交时刻、在解码输出回调按 `pts` 配对求差），
  `moonlightTelemetry()` 被调用时再读入瞬时 / 库侧读数（`LiGetEstimatedRttInfo` 的 RTT 与抖动、
  `LiGetRTPVideoStats` 的包计数、`LiGetPendingVideoFrames`）。**原生不产速率**：速率与每帧均值在 ArkTS
  （`services/MoonlightTelemetry`）按相邻样本差分 / 求均得到（字段与口径见
  [`moonlight-session.md`](moonlight-session.md) §2）。计数在每次会话 `start` 时清零，会话重启后
  ArkTS 检测到计数回落即重新基准化。

### 2.1 本地光标模式（基地版 Sunshine 的光标通道）

- **基线**：Moonlight 协议**没有光标通道**——`STREAM_CONFIGURATION` 无光标字段、库无光标回调，主机
  （GFE / 原版 Sunshine）把光标直接合成进编码画面。标准客户端（moonlight-android）因此只是隐藏本机
  指针、显示画面里的主机光标。
- **foundation-sunshine 的光标扩展**（HmRdp 已适配；改动写进 `native/scripts/patch-moonlight.ps1`）：
  - **能力广告**：RTSP DESCRIBE 的 `a=x-ss-general.featureFlags` 置位 `cursor_shape = 0x40`（仅当主机有
    光标生产者：Windows `ddx`/`vdd` 捕获）。客户端库已解析该值（`SunshineFeatureFlags`）。广告按
    **DESCRIBE 时刻**的 producer 存在性给出，而 producer 随**首条流**的显示捕获创建（`display_base.cpp`
    里置位）——**主机启动后的首连该位恒缺席**，此刻主机其实已能接受 enable（见下）。
  - **功能握手（客户端→主机）**：控制流消息 `0x5509`，payload 恰 4 字节 `[version=1, mode(0关/1开), 0, 0]`。
    仅在被**加密控制流 v2**（`controlProtocolType == 13`）接受；客户端用 `corever=1` 协商 v2。
    **客户端不按广告位过滤 enable**（广告位对首连是陈旧的，见上；只按 `IS_SUNSHINE` + 加密 v2 发送），
    因为主机在 enable 时会以「加密控制流 + producer 可用」**再校验**，拒绝则静默忽略——回退由此保证。
    enable 是**一次性握手**：协议无 ACK、无重传，丢包会让整段会话静默失去光标通道，故必须
    `ENET_PACKET_FLAG_RELIABLE` 发送（上游连周期 ping 都用可靠发送）。enable 被接受后，主机侧按
    latest-value 桥（`cursor_channel`）把当前形状随下一次发布送达，客户端无需拉取。
  - **主机→客户端**：同 `0x5509`，28 字节 LE 头 + BGRA 分片（每片 ≤60000B）：
    `u8 version, u8 flags(0x01 shape / 0x02 visible), u16 header_size=28, u32 shape_id, u16 w, u16 h,
    i16 hotx, i16 hoty, u32 total_size, u32 offset, u16 chunk_size, u16 reserved` + 片字节。
  - 启用后主机**不再把光标画进画面**，改为在 `shape_id` 变化时发一次形状（可能分片）、在可见性变化时发更新。
    协议是纯**推送**模型：客户端不能拉取，首个形状到来之前本机只有默认箭头；因此客户端把光标通道当
    **状态**维护（形状只发一次也要一直生效），并在窗口绑定 / 指针重进等时机重放（见
    [`session-layer.md`](session-layer.md) §10「重放点」）。
- **客户端实现分层**：补丁给 `Limelight.h` 加 `ConnListenerCursorUpdate` / `LiSetLocalCursorMode`，给
  `ControlStream.c` 加 `0x5509` 收发（`HMRDP_CURSOR` 标记；enable 幂等，重复发送安全）；
  `hmrdp_moonlight_session.*` 把回调转成原生光标回调，并**在 `LiStartConnection` 返回后**发 enable；
  **拆流前发 disable（`LiStopConnection` 之前、控制流仍在）**——模式由主机按连接持有，中断连接不会自行还原；
  `hmrdp_napi.cpp` 把形状（含 `shapeId`）经 `g_buffers` 句柄送到 ArkTS（事件第 4 个对象参数）；会话页把更新
  交给两协议共用的安装器 `services/RemoteCursor.ets`（去重键 = `shapeId` + 几何，倍率恒 1，见
  [`session-layer.md`](session-layer.md) §10「远端光标同步」）。设置项是 `mlCursorMode` 三选一（见
  [`moonlight-connection.md`](moonlight-connection.md) §5）。
- **回退**：主机拒绝 enable（非 Sunshine、非加密控制流 v2、或 enable 时仍无 producer，如 WGC 捕获）时
  静默忽略，客户端收不到形状 ⇒ 系统光标保持默认（等于「关闭」）。
- **对原版 Sunshine 的兼容性**：其控制流分发对未注册类型只做 debug 日志后丢弃（`stream.cpp`
  `control_server_t::call`），所以向原版主机发 enable 是无害 no-op（收不到形状即回退）；GFE 由
  `IS_SUNSHINE()` 挡住不发。客户端改动不引入任何协议层面的兼容风险。
- **验证**：需一台跑 foundation-sunshine 的主机（真机）。

### 2.2 原生触控板（基地版 Sunshine 的 precision touchpad 扩展）

> 归属见 [`session-layer.md`](session-layer.md) §8；这里是原生侧事实与接入。

- **基线**：Moonlight 协议**没有**触控板通道；标准做法是把触控板翻译成滚轮 / 高分辨率滚轮 + 鼠标
  （`LiSendHighResScrollEvent` 即为此）。
- **foundation-sunshine 的触控板扩展**（HmRdp 已适配；改动写进 `native/scripts/patch-moonlight.ps1`
  第 3 步，**不引 fork**）：
  - 输入流包 `0x55000009`（单触点）/ `0x5500000A`（整帧 ≤5 触点）；x/y 是**物理触控板表面**的归一化坐标，
    帧带 `deviceWidthMm/HeightMm` 与 `buttonState`；主机用 Windows 合成指针 `PT_TOUCHPAD`
    （`CreateSyntheticPointerDevice2`，Win11）注入，**捏合 / 滚动交给系统手势引擎原生处理**。
  - 能力位 `LI_FF_TOUCHPAD_EVENTS = 0x10` / `LI_FF_TOUCHPAD_FRAME_EVENTS = 0x20`，随 SDP
    `x-ss-general.featureFlags` 到达（`LiGetHostFeatureFlags`）。
  - `LiSendTouchpadEvent` / `LiSendTouchpadFrameEvent` 在能力位缺席时返回 `LI_ERR_UNSUPPORTED`，
    所以**一般 Sunshine / GFE 不受影响**（不会收到未知包）。
- **接线**：Moonlight 的捏合模式可选「原生转发」；选中且主机广告帧能力时，`SessionInput` 走
  `InputSink.touchpadFrame`（合成两个触点——鸿蒙不向应用暴露触控板真实触点），否则与「Ctrl+滚轮 / 触摸模拟」
  一样回退。N-API 出口 `moonlightTouchpad`。

### 2.3 音频（Opus 解码 → 交接环 → ArkTS 渲染器 / 原生 OHAudio 低延迟）

- **协议形态**：Sunshine / GFE 的音频是 **Opus 多流**（配置为 `MAKE_AUDIO_CONFIGURATION(channelCount, channelMask)`，
  如 `AUDIO_CONFIGURATION_STEREO`），`moonlight-common-c` 只分帧、**不解码**：`AUDIO_RENDERER_CALLBACKS.decodeAndPlaySample`
  交来的是**明文** Opus 包（解密在库内完成）。解码器因此由客户端提供：仓库固定 **libopus**（`opus_multistream_decoder`），
  随 `build-native.ps1` 编成共享 `libopus.so`（无版本号 SONAME），由 `libhmrdp` 链接（依赖清单见
  [`native-build.md`](native-build.md)）。
- **解码桥**：`core/hmrdp_moonlight_audio.*` 把 libopus 接进 `AUDIO_RENDERER_CALLBACKS`——`init` 按
  `OPUS_MULTISTREAM_CONFIGURATION`（`sampleRate/channelCount/streams/coupledStreams/mapping`）建多流解码器，分辨率不写死；
  `decodeAndPlaySample` 解出 **PCM S16LE** 写入与 RDP **同一个** `AudioSink` 交接环（`core/hmrdp_audio.*`）；
  `start`/`stop`/`cleanup` 驱动生命周期。能力位留空：**不开 `CAPABILITY_DIRECT_SUBMIT`**，解码跑在 moonlight 自己的
  `AudioDecoderThread` 上，不占 RTP 接收线程（与视频桥同理）。
- **与 RDP 共用播放链路**：环与 ArkTS 渲染器（`workers/audio.worker.ets` + 门面 `services/AudioPlayer.ets`）是 RDP 音频那一套；
  环本就**格式无关**（水位按 `rate × channels × 2` 算），立体声与环绕都不必改环。ArkTS 侧按**音源**分流到各协议的原生读口
  （RDP `audioRead(handle,…)` / Moonlight `moonlightAudioRead`，后者无 handle——环是进程级单个）。
- **低延迟模式（原生 OHAudio，可选、默认开）**：`mlLowLatencyAudio` 为真时音频不走 ArkTS 渲染器，改由
  `core/hmrdp_oh_audio.*` 建原生 `OH_AudioRenderer`（`AUDIOSTREAM_USAGE_GAME` + `AUDIOSTREAM_LATENCY_MODE_FAST`），
  写回调直接从同一个 `AudioSink` 环拉；环深度由设备回调周期派生（`OH_AudioRenderer_GetFrameSizeInCallback`，
  口径见 [`rdp-native.md`](rdp-native.md) §5）。库的 `AudioDecoderThread`（环的生产者）接入**音频工作组**
  （`AddCurrentThread` + 每帧 `Start/Stop`）保证及时供数——FAST 用小缓冲，供数不及时即补静音帧（杂音）。
  关时回到 ArkTS 渲染器那条链路。FAST 在设备/格式/资源不满足或有更高优先级流时由系统**回落普通通路**
  （`OH_AudioRenderer_GetFastStatus` 记录，不据此失败）；`enableAudio` 关时两条路都不建。
- **格式时序**：音频格式在 `init` 时确定（Opus 恒 48kHz，声道由协商定），且 `init`（在 `startAudioStream` 内）在
  `connectionStarted` **之前**执行 ⇒ ArkTS 收到会话 `kStarted` 时 `moonlightAudioFormat()` 已可读，**无需音频格式事件**
  （RDP 因 rdpsnd 格式可变才需要 `AudioFormat` 事件；Moonlight 不会中途重配）。
- **开关与协商落点**：`MoonlightStreamConfig.enableAudio` 关闭时 `Run()` 传空 `arCallbacks`（库回落假渲染器 ⇒ 静音）；
  `/launch` 的 `localAudioPlayMode` 随之取 0（音频送客户端）/ 1（主机本地播放）。`surroundAudioInfo` 与
  `STREAM_CONFIGURATION.audioConfiguration` 当前为立体声；环绕是后续扩展（鸿蒙 ArkTS 与原生 OHAudio 两侧的高声道布局接口均已具备）。
- **A/V 同步不在范围**：音频环独立播放，与其解码时间轴无耦合。

## 3. 范围

- 本文件覆盖：客户端库的构建与链接（§1）、桥接层与接线（§2，解码桥 + 串流会话桥 + 本地光标模式 + 原生触控板 + 音频）。
- **不在本文件范围**：视频解码与协商（[`moonlight-video.md`](moonlight-video.md)）、控制/配对层
  （[`moonlight-control.md`](moonlight-control.md)）、主机发现与应用列表交互（[`moonlight-apps.md`](moonlight-apps.md)）、
  会话的 ArkTS 接线与输入页路由（[`moonlight-session.md`](moonlight-session.md)）。
- **改前必读**：会话侧接线要遵守既有的线程模型，见 [`architecture.md`](architecture.md) §4。
