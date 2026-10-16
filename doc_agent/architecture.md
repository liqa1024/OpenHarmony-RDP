# 应用结构

## 1. 一句话数据流

```
连接列表(Index) ──选中──> SessionManager（主窗口后台连接）
                              │  解析显示/高级设置：SettingsStore.resolveDisplay / resolveAdvanced
                              │  取密码：CredentialStore(ASSET)
                              │  开窗：WindowController → SessionAbility（独立窗口，instanceKey）
                              ▼
                        SessionPage（XComponent 画面 + 输入 + 工具栏/遥测）
                              │  每会话一个 RdpNative 实例（Node-API → hmrdp_session）
                              ▼
                    原生：FreeRDP 客户端 → GFX → gdi 解码 → 呈现器上屏（Vulkan/GLES）
```

配置/密码/设置只在**连接建立时**读取，见 [`settings-and-storage.md`](settings-and-storage.md) §4。

## 2. ArkTS / ArkUI 职责表

| 路径 | 职责 |
|---|---|
| `entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置与连接存储、按默认尺寸创建主窗口、加载连接列表；关闭时若有会话窗口先弹窗确认再断开并关闭（见 [`session-layer.md`](session-layer.md) §7） |
| `sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸（或全屏）创建窗口、加载会话页；单窗口模式下拦截关闭以先恢复主窗口 |
| `pages/Index.ets` | 连接列表（点左侧→编辑，右侧圆钮→连接，右键菜单，拖拽排序，FAB 新建） |
| `pages/SessionPage.ets` | RDP 会话页：画面（`SessionPicture`）、协议专属浮层/工具栏、RDP 直达缩放的视图矩阵；窗口外壳（窗口/光标/工具栏自动隐藏/输入接线/遮罩）复用 `SessionShellController`；**输入事件交给 `SessionInput`**；转发下载事件、渲染进度（下载状态机在后台 worker，见 §4.4） |
| `pages/MoonlightSessionPage.ets` | Moonlight 会话页：画面（`SessionPicture`）先绑 surface 再 `SessionManager.beginStream`（`LiStartConnection` 要渲染目标）；窗口外壳复用 `SessionShellController`；**输入事件交给 `SessionInput`**；状态浮层 |
| `pages/MoonlightAppsPage.ets` | 主窗口内的 Moonlight 应用管理页：应用列表 / 运行状态 / 启停 / 默认应用（星标）/ 远程图标 |
| `pages/EditConnectionPage.ets` | 新增/编辑连接（保存只写配置；密码在连接成功后自动保存；可折叠「高级设置」） |
| `pages/SettingsPage.ets` | 全局设置（工具栏、光标、触控板/触屏、Win 键替代、分辨率与缩放、连接特性、窗口尺寸、主窗口自动隐藏、配置导入/导出） |
| `pages/GfxReplayPage.ets` | dev 页：码流回放测试（路线选择、统计显示），见 [`rdp-gfx.md`](rdp-gfx.md) §6 |
| `services/ConnectionStore.ets` | 连接配置存储（preferences；稳定 id + `updatedAt`；`reorder` 持久化顺序） |
| `services/CredentialStore.ets` | 密码存储（ASSET，按连接 id，**仅连接成功后写入**） |
| `services/SettingsStore.ets` | 设置存储（preferences）+ 显示/超分/串流解析（`detectedDisplay` / `scalePresetForResolution` / `autoScalePercent` / `resolveDisplay` / `resolveAdvanced` / `resolveSr(protocol, setting)` / `resolveStream`；超分能力按协议判，`srBackendUsable(backend, protocol)`，会话尺寸 `srSessionSize` 是唯一口径） |
| `services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；密码不导出） |
| `services/SessionManager.ets` | 主窗口后台连接、每连接状态、错误分类、成功后开窗与断连编排；按协议取 `SessionBackend`，自身不绑定协议（状态机见 [`session-layer.md`](session-layer.md) §5） |
| `services/SessionBackend.ets` | **协议无关的会话后端接口 + `sessionKey` 注册表**：`pageRoute()` 定会话页、`setShortcutPassthrough()` 统一快捷键拦截、`requiresPrepare()` / `requiresWindowAtStart()` 为能力查询 |
| `services/RdpSessionBackend.ets` | RDP 会话后端 `RdpSessionBackend`（包一个 `RdpNative`）；输入落地已移出，见 `services/input/RdpInputSender.ets` |
| `services/SessionBackendFactory.ets` | 按 `PendingConnection.protocol` 选后端（RDP / Moonlight），唯一后端组合入口 |
| `services/RdpLauncher.ets` | RDP 连接发起：装配显示 / 高级选项与密码后交 `SessionManager`（与 `MoonlightLauncher` 对称） |
| `services/ConnectionLauncher.ets` | 保存连接的**发起入口**：按连接的多态子类分发到 `RdpLauncher` / `MoonlightLauncher`（`afterDelete` 亦在此做协议善后），页面不再判断协议 |
| `services/SessionViewDelegate.ets` | 会话外壳与协议页之间的 host / hooks 契约（协议专属策略：输入装配、标题、事件与 surface、可见性、直达缩放、资源回收），外壳只调钩子、不含协议判断 |
| `services/SessionShellController.ets` | **两协议共用的会话外壳**：窗口生命周期 / 焦点与可见性聚合、`RemoteCursor`、`ToolbarAutoHide`、输入管线、遮罩计时、快捷键拦截、surface 回调；协议差异经 `SessionViewDelegate` 注入 |
| `services/SessionInput.ets` | **两协议共用的输入内核（瘦 Facade）**：只做编排（控制器调用顺序、对外 API、失焦全量释放汇总），策略在 `services/input/*`（见 [`session-layer.md`](session-layer.md) §8） |
| `services/input/*.ets` | 输入策略控制器：`PointerInput`/`KeyInput`/`TouchInput`/`TouchpadInput`/`WheelInput`/`PinchInput`（各关注点）+ `GamepadInput`（手柄，Moonlight 专有，附 `GamepadDevices` 设备枚举）+ `HeldInputs`（按住账本）/`InputPolicy`（时序几何常量 + 共享指针几何）/`InputSink`（类型与接口），以及两层各自的协议落地 `RdpInputSender`/`MoonlightInputSender`。控制器只把平台事件翻译成普通动词，**不自持投递策略**（限量在 `services/InputPort.ets` 的闸门） |
| `services/InputPort.ets` | **L1 输入派发器 `SessionInputPort`**：控制器的 sink，也是整条输入流水线**唯一的限量实现**（窗口 / 速率上限 / 丢弃计数 / 离散前的冲刷）；动词一律入队给输入 worker，策略查询按本地状态回答（见 [`session-layer.md`](session-layer.md) §8） |
| `services/InputProtocol.ets` | L1↔L2 的消息形状、投递类声明（每条流的窗口）与遥测形状 |
| `services/TouchpadTapFilter.ets` | 触控板「双指双击＝中键」判定器：只吃触控板右键的按下/抬起/移动，用按下→抬起时长区分轻触与实体右键（拖动），输出经 `TouchpadTapTarget` 交回 `PointerInput`；状态与定时器自持有 |
| `services/RdpNative.ets` | 每会话一个实例（独占原生 handle）；把事件交给 `RdpEventHub`，由 `RdpSessionBackend` 包进 `SessionBackend` |
| `services/RdpEventHub.ets` | RDP 原生事件总线的唯一订阅者：收到 native 事件后按 handle 路由给归属会话（见 §4.4） |
| `services/MoonlightSessionBackend.ets` | Moonlight 会话后端：包原生 `LiStartConnection`（`hmrdp_moonlight_session.*`），事件→`SessionSignal`，挂/放 XComponent surface；输入落地已移出，见 `services/input/MoonlightInputSender.ets`（含触屏 / 高分辨率滚轮 / UTF-8 文本 / 控制器 / 原生触控板接入点） |
| `services/MoonlightConnector.ets` | Moonlight 连接预检：`serverinfo →（pair）→ launch`，装配原生串流配置；PIN 由调用方弹窗提供；失效 pin 清理 |
| `services/MoonlightHostState.ets` | 主机「在跑哪个应用」的单一权威（静态监听器；客户端动作乐观写入，主机探测收敛），列表行与应用页徽标都由它派生 |
| `services/MoonlightLauncher.ets` | 构造 Moonlight 连接请求（解析分辨率/应用 id/`filesDir`/配对 PIN 弹窗）后交给 manager |
| `services/MdnsDiscovery.ets` | 通用 mDNS 主机发现：按 DNS-SD 服务类型发现并解析为 IP，去重成实时主机列表（`_nvstream._tcp` 即 Moonlight/Sunshine） |
| `services/WindowController.ets` | 窗口默认尺寸、拉起会话窗口、全屏与系统标题栏控制、单窗口模式的主窗口隐藏/恢复 |
| `services/DeviceCapabilities.ets` | 运行时能力探测（`Capability{supported, reason}`），供 UI 置灰 + 说明原因（模式见 [`rdp-native.md`](rdp-native.md) §6） |
| `services/SystemMaterial.ets` | 沉浸光感（系统材质）的统一入口：能力判定（首用时求值）、材质与兜底磨砂的复用、`ImmersiveModifier`（见 [`arkts-conventions.md`](arkts-conventions.md) §6） |
| `services/ToolbarAutoHide.ets` | 会话工具栏自动隐藏判定（指针分区 / 揭示停留 / 收起延迟 / 传输钉住），两协议会话页共用 |
| `services/RemoteCursor.ets` | 远端光标安装器，两协议会话页共用（去重、目标尺寸、热点、安装、捏合定位光标、可见性归属），见 [`session-layer.md`](session-layer.md) §10 |
| `services/RdpTelemetry.ets` | 工具栏遥测的 ArkTS 聚合：把原生原始样本换算成速率与每帧均值（口径见 [`rdp-session.md`](rdp-session.md) §5） |
| `services/MoonlightTelemetry.ets` | Moonlight 工具栏遥测的 ArkTS 聚合：把原生累计样本差分/求均成速率与每帧均值（字段与口径见 [`moonlight-session.md`](moonlight-session.md) §2，原生采样见 [`moonlight-native.md`](moonlight-native.md) §2） |
| `services/TouchpadWheel.ets` | 触控板/滚轮/引擎位移（**vp**）→ 高分辨率 RDP 轮转量映射（`TouchpadWheelMapper`，两轴各一个累加量）：名义锚点 `REMOTE_PX_PER_DETENT`（远端每齿 48px）+ 端到端标定倍数 `SCROLL_CALIBRATION`（=4，经验值），mapper 再按 `pxPerVp` 折成本机 vp 口径；三条输入都先落到 vp 再除它 |
| `services/BackgroundWorker.ets` | UI 线程侧的后台 worker 门面：按 id 匹配请求/应答、按 handle 路由事件（见 §4.4） |
| `services/BackgroundProtocol.ets` | 后台 worker 的消息形状（请求/应答/事件，见 §4.4） |
| `services/AudioPlayer.ets` | UI 线程侧音频 worker 门面：按音源（RDP handle / Moonlight 无 handle）start/stop，并把 native 的 `AudioFormat` 事件转发给音频 worker |
| `services/AudioProtocol.ets` | 音频 worker 的消息形状（请求/应答/事件，含 RDP/Moonlight 音源） |
| `workers/background.worker.ets` | **后台 worker（进程一条）**：剪贴板编解码 + 下载状态机/文件 IO（见 §4.4） |
| `workers/FileDownloader.ets` | 远端文件下载状态机（worker 侧）：分片请求、沙箱写盘、进度/完成/失败事件 |
| `workers/audio.worker.ets` | **音频 worker（进程一条）**：每会话一个 ArkTS `AudioRenderer`，其 `writeData` 经 `audioRead`（RDP）/ `moonlightAudioRead`（Moonlight）从原生 sink 环拉 PCM（见 §4.4） |
| `workers/input.worker.ets` | **输入 worker（每会话一条）**：输入的唯一发送者——消费 L1 投递的记录、调 L3 sender 并累计拒收；**不做窗口 / 等待**（限量全在 L1 闸门），见 §4.4 |
| `moonlight/MoonlightClient.ets` | Moonlight 控制层编排：`serverinfo → pair → applist → launch/resume`，并装配原生串流配置（见 [`moonlight-control.md`](moonlight-control.md) §1） |
| `moonlight/MoonlightIdentity.ets` | 客户端身份（自签名 X.509）+ HUKS 信封私钥 + 逐主机 pin 存储 |
| `moonlight/MoonlightCrypto.ets` | Moonlight 配对加密原语（AES-ECB、SHA、RSA 签验） |
| `moonlight/MoonlightHttp.ets` | Moonlight HTTP/HTTPS（mTLS、证书 pin）传输（`@ohos.net.http`） |
| `moonlight/MoonlightXml.ets` | Moonlight 控制端点响应的 XML 解析 |
| `moonlight/MoonlightModels.ets` | Moonlight 协议数据模型（serverinfo/applist/pair 等） |
| `utils/ConnectAction.ets` | `connectActionFor`：由会话状态与主机目标推出动作（协议中立），连接行与卡片共用 |
| `model/ConnectAction.ets` | 连接动作词汇与文案（`ConnectAction` / `connectActionLabel`），无依赖叶子模块 |
| `utils/KeyMapper.ets` | **两协议共用的单键表**：一次给出 RDP 的 `scancode`+`extended` 与 Moonlight 的 `vk`+`extended`（及 lock 位）；`describe` 供设置页按键框显示（见 [`session-layer.md`](session-layer.md) §8） |
| `utils/MoonlightKeyMapper.ets` | Moonlight 输入常量（`LiSend*` 的按键/按钮/action、modifier 位、`MODIFIER_EXTENDED`）与 VK→modifier 判定 |
| `utils/MoonlightCodec.ets` | Moonlight 编码器能力分类（硬解 / 软解 / 无） |
| `utils/ClipboardCodec.ets` | 剪贴板格式编解码（DIB/RTF/HTML/CfHTML 解析与组装），跑在后台 worker |
| `utils/FileTransferCodec.ets` | 文件剪贴板线格式编解码（`FileGroupDescriptorW` 等） |
| `utils/UiUtil.ets` | `showToast` / `showConfirm` / `showNotice` / `lengthToNumber` 等共享 UI 工具 |
| `utils/DisplayUtil.ets` | `defaultDisplay()`：容错查询默认显示 |
| `utils/ScaleParse.ets` | 手动分辨率 / 缩放文本解析与 `MAX_DESKTOP_SIZE` |
| `utils/PairingPrompt.ets` | Moonlight 配对 PIN 弹窗（四位数字居中放大，握手结束自动关闭） |
| `utils/HostSearchPrompt.ets` | mDNS 主机选择弹窗（只渲染已搜到的主机，已添加的置灰） |
| `model/SessionSignal.ets` | 协议无关的会话信号（生命周期 `Connected/Disconnected/Error` + 协议自带码的 `UiEvent`），manager 只消费 `kind` |
| `model/Connections.ets` | 协议无关的连接模型：`SavedConnection` **抽象基类**（只放共有字段 `id/updatedAt/host/port/name/protocol` 与共有行为）与 `RdpConnection` / `MoonlightConnection` 子类（各自持有本协议字段 + 图标、副标题、主连接动作、行菜单数据、记录 / 导出编解码）、`PendingConnection` / `SessionRequests` / `ConnectOrigin`、Moonlight 画质模型与 `ML_*` / `PROTOCOL_*` 常量、行菜单数据 |
| `model/RdpModels.ets` | RDP 专属模型：`RdpConnectOptions`（原生 `RdpOptions` 实现）、`RdpEvent` / `RdpPointerFlags` / `RdpTouchFlags` / `RdpLockFlags` / `RdpClipKind`、RDP 显示 / 超分 / 高级 profile 与 `SR_BACKEND_*` |
| `components/*.ets` | 共享 UI 组件（清单与参数契约见 [`ui-design.md`](ui-design.md) §4.1） |
| `entrybackupability/EntryBackupAbility.ets` | 系统备份/恢复 Ability |

## 3. 原生（`entry/src/main/cpp/`）

源码按职责分两层（`CMakeLists.txt` 把两层都加入 include 路径，头文件名保持不前缀）：

- **`binding/`** —— **Node-API 翻译层**：只做句柄/标量/小事件的编组与跨线程事件投递，不含业务逻辑；
- **`core/`** —— **原生能力层**：只保留「必须原生」的部分（判据见 §3.1）——资源驱动与数字热路径（FreeRDP 宿主、gdi 帧管线、GPU 呈现、编解码、音频、Moonlight 传输），以及 FreeRDP 在自有线程上要求**同步返回**的回调适配。

### 3.1 边界判据：什么必须留在原生

一个能力只有满足下面**任一条**才留在原生；不满足的一律属于 ArkTS（控制面、状态机、格式编解码、协议编排、遥测聚合）：

1. **API 只存在于原生**：FreeRDP、Vulkan / `OHNativeWindow`、`OH_AVCodec`、ffrt、HUKS、OpenSSL、moonlight-common-c、XEngine。
2. **回调必须同步返回或绑定线程**：FreeRDP 在自有线程上要求同步返回的回调（gdi `BeginPaint`/`EndPaint`、cliprdr、rdpsnd、pointer、autodetect）；跨到 ArkTS 再返回既无法同步，也无法保证线程。
3. **热数据路径的逐项穿越**：每帧 / 每音频样本一次 NAPI 调用会引入 JS 停顿与 GC。

第 3 条不构成「把逻辑放原生」的理由：跨边界的是**控制**而非**数据**——二进制留在原生、ArkTS 持句柄按需读取（`bufferRead`），遥测由 ArkTS 主动拉取原始读数（`sessionTelemetry`）并自行聚合，事件是离散的。热路径只要求「每条数据的搬运不逐项穿越边界」，不要求把聚合或状态机放原生。

**推论**：native 的职责只是**调用封装（`binding/`）+ 原生数据处理（`core/`）**，不承放应用逻辑；凡「因为看着像核心」而留在原生的状态机、聚合与格式处理，都应回到 ArkTS。判定候选是否该迁：把上面三条逐一对照，一条不满足即迁 ArkTS。

| 路径 | 职责 |
|---|---|
| `binding/hmrdp_napi.cpp` | Node-API 翻译层：类型化句柄注册表、结构化事件 + 计数拉取、位图缓冲句柄、各原语导出 |
| `binding/handle_registry.h` | 类型化句柄注册表（单调不复用、按类型隔离、查找交出 `shared_ptr`） |
| `core/hmrdp_session.{h,cpp}` | FreeRDP 客户端生命周期、输入、事件、光标位图解码（xor/and 掩码→BGRA，缩放策略在 ArkTS）、会话遥测；画面由 gdi 出、经呈现器上屏 |
| `core/hmrdp_bitmap.{h,cpp}` | 协议无关 BGRA 位图原语：预乘 alpha 面积平均缩小 / Lanczos3+锐化放大（`scaleBgraBitmap` 句柄进句柄出）+ FNV 内容哈希（光标去重键）；策略在 ArkTS `RemoteCursor` |
| `core/hmrdp_file_clipboard.{h,cpp}` | 文件剪贴板域：文件格式 id/名、上传进度载荷与文件大小工具（供上传 serve 用；下载状态机在 ArkTS） |
| `core/hmrdp_gfx_driver.{h,cpp}` | 原始 GFX 通道的离线回放泵（FreeRDP 自己的 ZGX + RDPGFX 解析）与解析计时 |
| `core/hmrdp_gfx_capture.{h,cpp}` | 原始通道录制与读取 |
| `core/hmrdp_gfx_cpu.{h,cpp}` | 离线 FreeRDP gdi 桌面（回放的解码器）；`PresentGdiFrame` 为 live 与回放共用 |
| `core/hmrdp_presenter.{h,cpp}` | **呈现器接口 + 后端选择**：按呈现能力选定 Vulkan 或 GLES（见 [`rdp-present.md`](rdp-present.md) §1） |
| `core/hmrdp_gles_presenter.{h,cpp}` | **GLES/EGL 兜底呈现器**：脏矩形 `glTexSubImage2D` + letterbox quad；每实例自有 EGL context/surface（只共享 EGL display） |
| `core/hmrdp_frame_pipeline.{h,cpp}` | 会话/回放的**帧管线**：presenter + gdi frame host + work meter + 解码执行器，全归其所有者（见 §4.1） |
| `core/hmrdp_gfx_work.{h,cpp}` | 帧工时计量器 `GfxWorkMeter`（live 与回放共用同一批钩子，口径见 [`rdp-session.md`](rdp-session.md) §5） |
| `core/hmrdp_display_rate.{h,cpp}` | **面板档位与送显槽位**：默认屏刷新率（首次读 + 显示变化监听）与"一个周期一帧"的槽位游标，两条上屏链路共用（见 [`rdp-present.md`](rdp-present.md) §2、[`moonlight-video.md`](moonlight-video.md) §1） |
| `core/hmrdp_parallel.{h,cpp}` | **解码执行器 `DecodeExecutor`**（每管线一个 ffrt 并发队列）+ `HmrdpParallelRun` 的 TLS 解析与串行兜底（见 [`rdp-gfx.md`](rdp-gfx.md) §7.3） |
| `core/hmrdp_replay.{h,cpp}` | dev 回放页：喂流、上屏、参考画面对比（见 [`rdp-gfx.md`](rdp-gfx.md) §6） |
| `core/hmrdp_vk_context.{h,cpp}` | Vulkan 运行时加载：`dlopen` + 标准能力探测（含超分后端与视频导入判定）+ 进程级**共享不可变** instance（surface 创建）+ 每 presenter 自有的 device/queue 与内存类型（device 按需使能交换链与外部内存/YCbCr 扩展，见 §4.1） |
| `core/hmrdp_vk_renderer.{h,cpp}` | Vulkan 上屏：**每 presenter 自有 `VkDevice`/queue**（从共享 instance 建）+ `VK_OHOS_surface` + swapchain + letterbox quad + 零拷贝桌面缓冲 + **超分辨率上采样 pass**；两种输入（桌面图 / 视频图）都只喂一个 `PresentInput` |
| `core/hmrdp_vk_video.{h,cpp}` | **Moonlight 视频导入**（`VkVideoImport`）：图形分配器缓冲按 **R8 字节图**外部内存导入 + 片元自己做 YUV→RGB（不依赖驱动的 YCbCr 转换）→ 会话分辨率的 picture；只对外 `Ensure / Import / Release / RecordConvert`，配方与判据见 [`rdp-present.md`](rdp-present.md) §5 |
| `core/hmrdp_video_consumer.{h,cpp}` | **Moonlight 解码输出的消费端**（`VideoFrameConsumer`）：拥有离屏 `OH_ConsumerSurface` 与**消费线程**，帧到达回调只发信号，线程内 acquire → 交 sink → **双缓冲归还**（见 [`rdp-present.md`](rdp-present.md) §5） |
| `core/hmrdp_xeg.{h,cpp}` | **超分辨率桥**：`dlopen` libxengine，探测 `XEG_spatial_upscale` 并封装上采样对象的创建/下发/销毁（XEngine 头文件只在本文件内引用） |
| `core/hmrdp_moonlight*.{h,cpp}` | Moonlight 原生：客户端库探针、视频解码桥、音频 Opus 解码桥（控制层、配对、身份与 pin 全在 ArkTS，见 [`moonlight-native.md`](moonlight-native.md) §2.3 / [`moonlight-control.md`](moonlight-control.md) §1） |
| `core/hmrdp_audio.{h,cpp}` | **两协议共用**的**有界 PCM 交接环**（RDP rdpsnd sink 与 Moonlight Opus 解码都写入它；消费端为 ArkTS 渲染器或原生 OHAudio，缓冲策略与延迟预算在环里，见 [`rdp-native.md`](rdp-native.md) §5） |
| `core/hmrdp_oh_audio.{h,cpp}` | **原生 OHAudio 输出**（Moonlight 低延迟模式）：`OH_AudioRenderer` FAST + 音频工作组，写回调直接拉交接环（见 [`moonlight-native.md`](moonlight-native.md) §2.3） |
| `core/hmrdp_gamepad_kit.{h,cpp}` | **手柄轴来源（Game Controller Kit 薄封装）**：注册设备的上下线与五个轴监听，回调把"哪个控制产生的轴事件"+值交给 N-API（见 [`session-layer.md`](session-layer.md) §8） |
| `core/hmrdp_video_decoder.{h,cpp}` | `OH_AVCodec` 硬件优先视频解码器（**surface 唯一输出**：直出窗口或超分消费端 surface；送显槽位在这里判，是全链路唯一的门控） |
| `core/hmrdp_decode_tuning.{h,cpp}` | 在线核数/性能簇/频率探测，供解码宽度与日志用 |
| `core/hmrdp_log.h` | hilog 包装（domain `0xD001`、tag `HmRdpNative`） |
| `shaders/present_quad.*` + `cmake/EmbedSpirv.cmake` | GLSL → SPIR-V 的构建期编译/嵌入（仍在 `cpp/` 根） |

## 4. 线程模型与所有权（原生）

### 4.1 所有权契约

会话是一条**自洽的数据管线**：管线上的资源全部归该会话所有；进程级只保留不可变对象，以及"函数指针全局、但按各自 context 路由到归属会话"的一次性钩子。

| 归属 | 资源 | 生命周期 |
|---|---|---|
| **会话私有** | FreeRDP context 及其内部线程、会话控制线程、帧管线（presenter 及其自有 VkDevice/queue、gdi 桌面与 GdiFrameHost、GfxWorkMeter、解码执行器、GFX 工作线程、观察者）、AudioSink；**Moonlight 会话的 presenter（超分时）与消费端 surface + 消费线程** | 随会话创建/销毁，**只在一处销毁** |
| **进程共享、不可变** | VkInstance（+ 能力探测）、日志、`g_entryPoints`/addin 注册、只读用户设置 | 只建一次，进程存活期不销毁 |
| **进程共享、按 context 路由** | audio sink、gfx raw capture、thread QoS applier | 一次性注册，调用时由 context 找到归属 |

**不存在进程级共享可变状态**：presenter 的 GPU device/queue、解码执行器、work meter 都随会话走。多会话并发因此既不互相阻塞，也不互相破坏——这是本文件最重要的约束。

### 4.2 线程

会话的并发结构是固定的，改任何与通道/渲染相关的东西前先对号入座：

| 线程 | 跑什么 | 约束 |
|---|---|---|
| RDP 传输线程（主） | 读 socket、解密、解复用；把 DVC 数据投进 drdynvc 队列；SVC（剪贴板、静态通道）回调；每秒一次遥测采样 | 不做长任务，否则拖慢整条链路 |
| drdynvc 分发线程 | FreeRDP 所有**动态通道**的分发：RDPGFX、rdpsnd（音频）、RDPEI（触屏/笔）、其它 DVC | **只准做轻活**（拆包 + 拷贝）；任何可能长时间占用的处理都必须交给应用线程。rdpsnd 在这里只重装 + 投递到它自己的队列 |
| **GFX 工作线程**（会话自有） | 整条帧流水线：ZGX 解压、PDU 解析、解码、合成、上屏、帧回执 | 帧重活只在这里；drdynvc 线程注册 sink 后只把通道数据拷进来，工作线程用 `HmrdpGfxReplayRecv` 重新进入 |
| **解码执行器**（会话自有 ffrt 队列） | progressive tile chunk 解码（每 region 若干任务） | 执行器随会话创建/销毁；解码入口以线程局部指针告知当前执行器，未设置则回退串行 |
| **rdpsnd 播放线程**（FreeRDP async `play_thread`） | rdpsnd PDU 处理：DSP 解码 → 交给 `AudioSink` 的交接环 | 不碰画面；缓冲/丢弃/预缓冲全在环一处（见 [`rdp-native.md`](rdp-native.md) §5） |
| **Moonlight 音频解码线程**（library `AudioDecoderThread`） | `decodeAndPlaySample`：Opus 多流解码 → 交给**同一个** `AudioSink` 交接环 | 不设 `CAPABILITY_DIRECT_SUBMIT`，故不占 RTP 接收线程；缓冲/预缓冲同样全在环一处（见 [`moonlight-native.md`](moonlight-native.md) §2.3） |
| **Moonlight 消费线程**（会话自有，`core/hmrdp_video_consumer.*`） | 超分时消费离屏表面的帧：acquire → 交 sink 上屏 → 归还（上一帧） | 表面自带的帧到达回调**只发信号**（不在其中调用该模块接口）；presenter 的调用在 sink 内由交接锁保护，故拆会话不会把 presenter 从在飞的帧下面抽走（[`rdp-present.md`](rdp-present.md) §5） |
| **OHAudio 回调线程**（Moonlight 低延迟模式，系统侧） | `OH_AudioRenderer` 的写回调直接从 `AudioSink` 环拉 PCM（`core/hmrdp_oh_audio.*`，见 [`moonlight-native.md`](moonlight-native.md) §2.3） | 回调内只做环的 `Read`（锁极短），**禁止**耗时操作与音频流控调用 |
| **（默认音频渲染在 ArkTS 音频 worker）** | 其余情况（RDP、Moonlight 关低延迟）渲染器的拉取在 ArkTS 音频 worker（`AudioRenderer.writeData` → `audioRead`/`moonlightAudioRead`，§4.4）；原生侧只由上面两条线程往 sink 环写入 | — |
| ArkTS/UI 线程 | 界面与输入事件；调用 `Send*` 写输入 | 不碰渲染/通道分发 |

**规则**：drdynvc 分发线程是**所有动态通道的公共瓶颈**——帧流水线一旦内联在它上面，一个重帧就会把音频、输入一起挡住（表现为音频卡顿、且与帧率绑定）。因此帧重活必须通过 sink 钩子交到应用线程（`HmrdpSetGfxDataSink` → 会话 GFX 工作线程，见 [`rdp-native.md`](rdp-native.md) §3）。新增/修改动态通道处理时按这条办。sink→队列这一段只拷不阻塞（队列是**延迟缓冲、不是节流**）：它不按帧数设硬上限，只在越过防御性 high-water（64MB）时打一次警告——**不能靠丢弃来限流**，每个 chunk 都是 GFX 流的一部分，丢了会破坏解析。

**分层原则**：真正的并发模型在 ArkTS 侧——页面/服务可按需为自己的缓冲、编排、状态机起独立线程；原生只保留**必须同步返回、或必须绑在库自有线程上**的适配器（文件上传 serve、rdpsnd sink、gdi / cliprdr / pointer 回调）。原生线程因此保持**简单且相互独立**：只做"拆包 + 拷贝 + 同步应答"，不承载应用状态机与缓冲策略。判定某个处理该放哪一侧时，先看它是否被这三类适配器"钉住"（§3.1 只是把同一判据用在功能层）：钉住则原生，否则归 ArkTS。

### 4.3 拆除（单一状态机）

拆除由会话控制线程独占，顺序固定：`kRunning → kStopping`（摘 sink 钩子、停止收块）`→ kDraining`（join worker、quiesce 执行器、detach surface）`→ kFreed`。具体顺序：停收流 → join worker → 卸载包装器 → `freerdp_disconnect`/join 内部线程 → 销毁管线（执行器 → presenter/device → 桌面）→ 释放 context。offload 后的 worker 与通道拆除不同线程，而 `freerdp_disconnect` 会释放 rdpgfx 插件，worker 必须在此之前 join（否则解析进已释放插件）。拆除不依赖超时兜底：有界等待只作诊断。

**服务端侧**：音频（rdpsnd）与输入（rdpei）是**独立虚拟通道**，图形流控（帧回执，MS-RDPEGFX 3.2.5.13）只作用于图形管线，服务端**不会因为等待客户端的帧而停发音频**；两边唯一共享的是 TCP 链路本身（全局带宽，与帧率无关）。

### 4.4 ArkTS 侧线程模型（UI 线程 / 后台 worker / 音频 worker）

并发只为**让 UI 线程不被重活堵住**；线程数按"互相独立、不可互相阻塞"的需要定，不按功能堆。原生侧只留被同步回调钉住的适配器（§3.1）。

- **UI 线程**：界面、事件路由、**输入的规范化与入队**（平台事件 → 控制器 → `SessionInputPort`，协议发送在输入 worker，见下）；以及边界上**唯一必需**的那次 native→ArkTS 拷贝（`bufferRead`——ArkTS 看不到原生内存，与线程无关）。剪贴板**格式协商的取舍**（选哪个格式、单在途、延迟刷新）也在这里：它只是一次事件上的小状态机，不含重活（编解码/写盘在后台 worker，见 [`rdp-clipboard.md`](rdp-clipboard.md)）。
- **后台 worker（一条）**：非实时的长驻状态机与重活——剪贴板 DIB/RTF/HTML 编解码、**本机剪贴板编码 + 原生推送**（native 会把整块内容拷进自己的存储并立即 advertise，一次通道发送，不能占 UI 线程）、文件下载状态机与写盘。彼此可互相等待，共用一条即可。落地：`workers/background.worker.ets` + UI 门面 `services/BackgroundWorker.ets`（消息形状 `services/BackgroundProtocol.ets`，下载状态机 `workers/FileDownloader.ets`）。
- **音频 worker（一条）**：ArkTS `AudioRenderer` 的回调需及时服务，**独占线程**；与后台 worker 共用会变成"音频卡顿且与处理绑定"（同 §4.2 的 drdynvc 教训）。落地：`workers/audio.worker.ets` + UI 门面 `services/AudioPlayer.ets`（消息形状 `services/AudioProtocol.ets`）。详见 [`rdp-native.md`](rdp-native.md) §5。
- **输入 worker（每会话一条）**：协议侧的发送成本不对称（RDP 是调用线程上的同步网络写，Moonlight 是入库返回），把它们从 UI 线程摘走，UI 线程就只做规范化与入队。落地：`workers/input.worker.ets` + L1 派发器 `services/InputPort.ets`（消息形状与投递类声明 `services/InputProtocol.ets`）；sink 在 worker 内构造，靠**进程级句柄注册表 / 库的进程级会话**直接落到原生（同「Worker 可调用原生」）。**入队即唤醒**，限量（窗口与速率上限）只在 L1 闸门（见 [`session-layer.md`](session-layer.md) §8），所以这条线程不轮询、不睡眠、也没有自己的窗口；它只逐条调 sender 并回报受理 / 拒收。

**事件路由**：native→ArkTS 全部事件走单个进程级 TSFN，回调固定在订阅它的 UI 线程运行时（`OnSessionEvent`）。`services/RdpEventHub` 是其唯一订阅者，按 handle 路由给 `RdpNative`；需 worker 消费的事件由 UI 线程按 handle `postMessage` 转发（Worker 拿不到该 TSFN：回调只跑 UI 线程，且各 Worker 有独立 `napi_env`）。Worker 只回控制/进度。

**事件载荷是类型化字段**：原生按 `Session::EventData`（整数槽 `values[]` + 文本 `text` + 二进制 byte 向量）产出，绑定层按事件映射成命名字段——跨边界不做 `a|b|c` 之类的字符串编组，因此两侧没有"同一套分隔约定"需要对齐。

**Worker 可调用原生**：Worker 内 `import 'libhmrdp.so'` 加载同一份 `.so`，进程级句柄注册表跨运行时共享——UI 线程 `createSession` 的 handle 可在 worker 直接用（读该会话原生缓冲）。前提：原生**不缓存 `napi_env`/`napi_value`**（唯一例外是绑 UI 线程的 TSFN）。

**拷贝纪律**：跨线程 `ArrayBuffer` 默认**转移、零拷贝**（发送方 detach）；普通对象序列化拷贝、单次上限 16MB ⇒ 大二进制走裸 `ArrayBuffer`，不放对象成员。除了按句柄读原生缓冲的两次拉取（UI 线程的 `bufferRead`、音频 worker 的 `audioRead`）外不为跨线程自加拷贝；`postMessage` 只承载控制/进度。

## 5. 其它

- `native/scripts/*` —— 原生库构建与 FreeRDP / Moonlight / zlib / opus 补丁（见 [`native-build.md`](native-build.md)）。
- `entry/libs/<abi>/` —— 本地构建的**全部三方共享库**（FreeRDP / OpenSSL / zlib / opus / moonlight，**不入库**）。
- `resources/base|dark/` —— 主题色与图标。
