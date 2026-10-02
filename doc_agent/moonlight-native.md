# Moonlight 原生客户端

本文件是 **Moonlight / Sunshine 串流的原生侧**知识：客户端库的拉取/补丁/构建与静态链接、视频解码与上屏、
桥接层约定、HTTP 控制层、客户端身份与信任存储与配对加密原语，以及后续的串流会话与输入接线。**协议编排与
全部 XML 解析在 ArkTS**（§4.2），不在本文件范围。**前端**侧（协议建模、存储布局、连接列表与
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
- **构建命令**（在 FreeRDP 那套之后）：

  ```
  native/scripts/fetch-sources.ps1     # 拉取并按 commit 固定；自动跑 patch-moonlight.ps1
  native/scripts/build-moonlight.ps1   # Windows NDK 交叉编译，静态归档
  ```

- **静态链接，不单独打包**：与 FreeRDP 不同，它没有运行时加载插件、不需要导出符号，因此构建成静态库
  （`-DBUILD_SHARED_LIBS=OFF`），连同 enet、OpenSSL 的 `libcrypto.a` 一起链进 `libhmrdp.so`；
  `entry/libs/<abi>/` 里**不新增 `.so`**。两者都带 `-fPIC`。
- **归档与头文件位置**：`build-moonlight.ps1` 不能依赖 `cmake --install`（上游 CMakeLists 不写 install 规则），
  它把 `libmoonlight-common-c.a` / `libenet.a` / `libcrypto.a` 与公开头 `Limelight.h` 收集到
  `native/install/<arch>/moonlight/`，再由 `sync-moonlight.ps1` 同步到
  `entry/src/main/cpp/thirdparty/moonlight/{include,lib/<abi>}/`（生成目录，**不入库**，与 FreeRDP 头文件同策）。
  `sync-moonlight.ps1` 按 arch 只替换 `lib/<abi>`，两个 ABI 可共存。
- **OpenSSL 依赖**：只用 `libcrypto`（配对 AES），复用它已经静态构建的那一份；构建时显式传
  `OPENSSL_ROOT_DIR` / `OPENSSL_CRYPTO_LIBRARY` 等，使 `find_package(OpenSSL 1.0.2 REQUIRED)` 命中
  OHOS 版本而不是宿主。
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

## 3. 桥接层与降级

- **降级式接线**：`entry/src/main/cpp/CMakeLists.txt` 只在
  `thirdparty/moonlight/lib/<abi>/libmoonlight-common-c.a` 存在时才定义 `HMRDP_HAVE_MOONLIGHT` 并链接；
  缺归档时 `hmrdp_moonlight.*` 编译成"不支持"，构建照常（同 FSR / XEngine 的降级写法）。因此未跑原生构建的
  检出仍能 `build_project`。
- **静态库必须真被引用才算连上**：静态归档里没被引用的目标不会进 `.so`。`hmrdp_moonlight.cpp` 的探针
  真正调用一个 moonlight 入口（`LiInitializeStreamConfiguration`，纯字段初始化、不起线程/不建 socket），
  所以"符号已解析"由**链接器**保证：有未解析符号则模块链接失败。
- **N-API 探针**：`moonlightProbe` 回 `"1"`，或未构建该库时回 `"0|no-moonlight"`（稳定原因码，中文文案归
  UI 层，同 [`native-libraries.md`](native-libraries.md) §6 的能力探测模式）。
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

## 4. GameStream HTTP / 配对层

- **一个原语**：`hmrdp_moonlight_http.{h,cpp}` 只做一件事——一次 HTTP/1.1 请求（TLS 或明文），返回**未解析的响应
  体**。`/serverinfo`、`/pair`、`/applist`、`/launch`、`/resume` 都由它拼装。**请求编排与全部 XML 解析都在 ArkTS**
  （`@ohos.xml`），原生不解析 XML，只提供"传输 + 证书/凭证 + 加密原语"——分层理由见 §4.2。
- **不复用 `freerdp_http_request`**：它端口写死 443、走系统 CA、只发 form-urlencoded、没有客户端证书——
  本层需要可配端口（47989/47984）、自签名主机与 mTLS。FreeRDP gateway 的 `http.c` 是 `FREERDP_LOCAL`
  （未导出），同样不可用。
- **OpenSSL 只同步头、不链库**：`native/scripts/sync-openssl.ps1` 只把本地 OpenSSL 构建的 `include/` 复制到
  `thirdparty/openssl/include`。TLS / RSA / AES / SHA 的**代码**用 `libwinpr3.so` 已静态包含并导出的那一份
  （FreeRDP 把 OpenSSL 静态编进 winpr3，符号默认可见 ⇒ 导出；`nm -D libwinpr3.so` 可见 `SSL_*`/`EVP_*`/
  `X509_*`）。再链一份 `libssl.a`/`libcrypto.a` 只会让进程里多一份 OpenSSL、并让 HAP 变大——链接器在任何
  顺序下都先绑到 winpr3。`moonlight-common-c` 的配对 AES 也落在这份上，故 `thirdparty/moonlight/lib/` 不带
  `libcrypto.a`。
- **信任模型 = 证书 pin**：未配对时 `pinnedServerCertDer` 为空 ⇒ 接受任意主机证书（首触，与官方客户端一致）；
  配对后存下**叶证书 DER**，之后每次请求要求逐字节一致。响应总是回报对端叶证书 DER（`serverCertDer`）供
  存储。`SSL_CTX_set_cert_verify_callback` 替换默认链校验是**有意**的：主机自签名，信任靠 pin 而非 CA。
- **mTLS**：已配对请求带客户端证书 + 私钥（PEM，`clientCertPem`/`clientKeyPem`）。
- **帧**：请求带 `Connection: close`；响应按 `Content-Length`，否则按 chunked，再否则以对端关闭界定。每请求
  一个新 `SSL_CTX` + socket，无进程级状态，可并发。
- **错误用稳定码**（`response.error`，中文文案归 UI）：`no-http` / `request` / `resolve` / `connect` / `tls` /
  `io` / `timeout` / `http`。
- **能力探针**：`moonlightHttpProbe` 回 `"1"`，未构建该层时回 `"0|no-moonlight-http"`（同其它能力探测模式）。

### 4.1 客户端身份与信任存储（`hmrdp_moonlight_cred.*`）

- **客户端身份**：自签名 X.509，RSA-2048、`SHA256withRSA`、20 年、随机序列号、`CN=NVIDIA GameStream Client`
  （与官方客户端一致；Sunshine 忽略 CN，GFE 需要它），证书输出为 **PEM 且 LF 换行**（CRLF 会被 PC 端拒收）。
- **私钥落盘用 HUKS 信封加密**：HUKS 内生成一把**不可导出的 AES-256-GCM 密钥**（按别名），私钥 PKCS8 用它加密
  后落 `client.key.enc`（`HMRDPML1` 魔数 + nonce + 密文 + GCM tag）；用时解密进内存再交 OpenSSL。证书是公开
  数据，明文落盘。
- **为什么不是 `certManager`「应用证书凭据」或 ASSET**：`certManager` 与 HUKS 的设计都是**私钥不可导出、只给
  签名**，而配对后的 applist/launch 走 **HTTPS mTLS**，原生 OpenSSL 必须拿到私钥字节来签握手——直接冲突
  （除非自写 OpenSSL provider，超出范围）。ASSET 容量 **≤ 1KB**，放不下 RSA-2048 的 PKCS8 私钥（≈1.2KB），
  官方也明说超过 1KB 改用 HUKS 或加解密框架。⇒ 取"HUKS 包裹的可读私钥"这一折中，私钥静态加密、运行时可读。
- **服务器证书 pin 按 host 存** `server_<key>.der`（DER，公开数据）。未配对接受任意主机证书，配对后逐字节
  比对叶证书 DER（§4 信任模型）。
- **目录**：`<dir>/moonlight/`，`dir` 是应用沙箱 files dir，由 ArkTS 传入。
- **损坏判定**：读回时校验魔数与 GCM tag，任一不过即视为损坏——删除 HUKS 密钥、重建身份并**清空全部 pin**
  （强制重新配对）。HUKS 密钥随应用卸载销毁，残留密文因此不可解密，走同一条重建路径。
- **依赖**：链接 `libhuks_ndk.z.so`（设备核心安全服务）。
- **能力探针/入口**：`moonlightCredentialProbe` / `moonlightEnsureCredential(dir)` / `moonlightClearCredential(dir)`。

### 4.2 配对：原生加密原语 + ArkTS 编排

- **分层与官方实现一致**：Qt 的 `NvHTTP::getXmlString` 用 `QXmlStreamReader`，Android 的 `NvHTTP.getXmlString`
  用 `XmlPullParser`；**配对编排（`NvPairingManager` / `PairingManager`）与普通端点共用同一套 XML 解析器、同在一个
  应用层**。本仓库照此办理：**配对编排与所有 XML 都在 ArkTS**，原生退为加密原语 + 通用 HTTP + 凭证/pin。**不在原生
  手写标签扫描**——那是参考实现里不存在的方言；OHOS sysroot 也没有 expat/libxml，原生用"真解析器"还得新增三方依赖。
- **时序**：**客户端生成并显示 4 位 PIN**，用户在**主机**上输入；`getservercert` 会等到主机端完成输入，故该步
  **无读超时**（其余步骤 5s 量级），由 ArkTS 用长超时发起。
- **步骤**（前四步走 **HTTP 47989**，最后一步走 **HTTPS 47984**）：
  `getservercert` → `clientchallenge` → `serverchallengeresp` → `clientpairingsecret` → `pairchallenge`（HTTPS +
  mTLS）。失败时 ArkTS 尽力 `/unpair` 清掉主机端半程状态。
- **原生原语**（`hmrdp_moonlight_pair.*`，hex 进 hex 出）：`moonlightPairDeriveKey`
  （`aesKey = SHA256/SHA1(salt‖pin)[0:16]`，主版本 < 7 用 SHA-1）、`moonlightAesEcb`（AES-128-ECB、零填充、无 IV）、
  `moonlightPairChallengeResponse`（哈希补零到 32 再 AES-ECB）、`moonlightPairExpectedResponse`（PIN 校验哈希）、
  `moonlightCertSignature`（证书的签名值字节）、`moonlightSignClientSecret`（`SHA256withRSA`）、
  `moonlightVerifyServerSecret`、`moonlightRandomHex`。哈希里串入的是**证书签名值字节**（`X509_get0_signature` 的
  ASN.1 BIT STRING 内容），不是公钥。
- **HTTP 客户端**支持**明文 HTTP**（`useTls=false`，47989 的 serverinfo / pair / unpair）与 TLS（配对后端点）；明文
  不带 pin、不带客户端证书，TLS 带存储的客户端证书并按 host pin（配对过程中可用 `pinServerCertHex` 覆盖）。
- **结果判定在 ArkTS**：PIN 错、`<plaincert>` 为空（另一设备在配对）、验签失败（MITM）等；成功后调
  `moonlightStorePinnedServerCert` 落 pin。

## 5. 范围与后续方向

- 本文件覆盖：客户端库的构建与链接（§1）、视频解码与协商（§2）、解码桥与降级（§3）、HTTP / 配对层（§4）。
- 超分链路（buffer 模式 + Vulkan 导入 + 复用 `VkRenderer`）只在真机验证，属独立后续里程碑，复用约束见 §2
  「与 RDP 呈现器的复用口径」。
- 主机发现、应用列表交互、串流会话建立（`LiStartConnection`）与输入接线不在本文件范围；**协议编排（驱动
  `serverinfo → pair → applist → launch`）与 XML 解析也在 ArkTS**，不在本文件范围。解码桥与原生控制层把两侧备好，
  编排接上后即可跑。路线与阶段见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)。
- **改前必读**：会话侧接线要遵守既有的线程模型（动态通道分发线程只做轻活，帧重活走 sink 钩子），见
  [`architecture.md`](architecture.md) §4。
