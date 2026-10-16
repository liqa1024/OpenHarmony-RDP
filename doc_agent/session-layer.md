# 两协议通用层（会话与连接）

本文件是 **RDP 与 Moonlight 共享的会话 / 连接层**的家：连接的协议建模与多态、连接发起分发、会话后端契约、
会话窗口外壳、共享输入内核，以及**会话窗口 / 输入 / 工具栏 / 光标安装器的共享行为**与**通用层的命名边界**。

协议专属内容不在这里：RDP 会话 / 输入 / 遥测见 [`rdp-session.md`](rdp-session.md)，RDP FreeRDP / 音频 /
能力见 [`rdp-native.md`](rdp-native.md)，GFX 见 [`rdp-gfx.md`](rdp-gfx.md)，上屏见
[`rdp-present.md`](rdp-present.md)，剪贴板见 [`rdp-clipboard.md`](rdp-clipboard.md)；
Moonlight 见 [`moonlight-connection.md`](moonlight-connection.md) / [`moonlight-apps.md`](moonlight-apps.md) /
[`moonlight-session.md`](moonlight-session.md) / [`moonlight-control.md`](moonlight-control.md) /
[`moonlight-video.md`](moonlight-video.md) / [`moonlight-native.md`](moonlight-native.md)；
职责表与线程模型见 [`architecture.md`](architecture.md)。

## 1. 连接模型：抽象基类 + 协议子类

- `model/Connections.ets` 的 `SavedConnection` 是**抽象基类**，只持有两协议**共有的字段**（`id` / `updatedAt` /
  `host` / `port` / `name` / `protocol`）与共有行为（`label` / `displayName` / `targetLabel`）；协议行为以抽象方法
  声明——`protocolIcon` / `subtitle` / `primaryAction` / `showsAppsButton` / `hostTargetId` /
  `defaultTargetId` / `menuEntries`。
- `RdpConnection` / `MoonlightConnection` 两个子类各自持有**本协议的字段**（RDP：账号 / 域 / 网关 / 显示 / 超分等；
  Moonlight：`ml*` 画质与主机身份等）并 override 行为；`SavedConnection.create(protocol)` 是**唯一实例化入口**。
  反序列化 / 导入 / 编辑页都按 protocol 建对应子类，再用 `instanceof RdpConnection` / `MoonlightConnection` 收窄后
  访问协议字段（`ConnectionStore.deserialize`、`ConfigTransfer`、`EditConnectionPage`、`SettingsStore.resolve*`）。
- **位置分隔存储 / 导出布局与协议无关**：子类各自实现 `toRecord` / `readRecord`（存储记录）与 `toExport` /
  `applyExport`（配置导入导出）里的**自己那段**，`ConnectionStore` / `ConfigTransfer` 只调这些多态方法、
  **不做协议判断**；抽象基类因此不继承任何协议专属类型，编码服务也不含协议分支。
- 由此，**列表行 / 卡片 / 列表页只调多态方法，不判断协议**：图标、副标题、主连接动作与行菜单都由连接对象给出。
- 连接动作词表 `ConnectAction` + 文案 `connectActionLabel` 在 `model/Connections.ets`；
  由会话状态与主机目标推出动作的 `connectActionFor` 在 `utils/ConnectAction.ets`（协议中立）。
- RDP 专属模型（`RdpConnectOptions`（原生 `RdpOptions` 实现，由 `RdpConnection.toOptions()` 生成）、`RdpEvent` /
  `RdpPointerFlags` / `RdpTouchFlags` / `RdpLockFlags` / `RdpClipKind`、显示 / 超分 / 高级 profile）在
  `model/RdpModels.ets`；**共用模型与 RDP 模型分文件**。

## 2. 连接发起：唯一分发点

- `services/ConnectionLauncher` 按连接的多态子类分发到 `services/RdpLauncher` / `services/MoonlightLauncher`；
  删除连接的协议善后（如 Moonlight 解配对）走 `ConnectionLauncher.afterDelete`。
- 会话后端组合由 `services/SessionBackendFactory` 按 `PendingConnection.protocol` 选
  `RdpSessionBackend` / `MoonlightSessionBackend`。
- 这两个入口是通用层里**唯一允许出现协议判断**的地方（组合根）。

## 3. 会话后端契约

- `services/SessionBackend.ets` **只有接口与注册表**（`SessionBackendRegistry`）：`pageRoute()`、
  `requiresPrepare()` / `prepare()`、`requiresWindowAtStart()`、`start()` / `stop()`、`handleId()`、
  `bindSurface()` / `clearSurface()`、`setShortcutPassthrough()`、信号监听与输入动词。
- **能力查询即多态**：`requiresPrepare()` / `requiresWindowAtStart()` 由后端自述，`SessionManager` 据此
  编排流程，**不出现协议名**。
- **连接过程两端同形：先发布 `Connecting`，再做异步预检**。`connect`/`retry` 一律**先发布 `Connecting`**
  再交给后端——列表行因此立刻进入加载态，与协议无关。后端可声明 **`requiresPrepare()`** 并在
  `prepare(): Promise<boolean>` 里做异步预检（Moonlight：`serverinfo → pair → launch`，配对 PIN 弹窗在此
  弹出）；预检期间 `Connecting` 已成立，所以不会出现"先无反馈、窗口里才闪一下加载"的断档。预检失败由
  后端发 `Error` 信号（文案即连接失败原因），窗口不开；`prepare()` 返回 `false` 而未发信号时由 manager 兜底
  （发布 `Failed`、停后端）。预检**不挂连接超时**（配对要等用户输入），超时只在真正开始启动后才计。
- 后端另可声明 **`requiresWindowAtStart()`**（Moonlight：`LiStartConnection` 需要先有渲染目标）——预检
  通过后 manager 才开窗，窗口绑好 XComponent surface 后调 `SessionManager.beginStream` 真正启动；RDP
  为 false，维持"连上后才开窗"。
- 协议实现各自成文件：`services/RdpSessionBackend.ets`（`RdpSessionBackend`）与
  `services/MoonlightSessionBackend.ets`（`MoonlightSessionBackend`）——只含后端本身，
  输入落地在 `services/input/RdpInputSender.ets` / `MoonlightInputSender.ets`（见 §8）。

## 4. 会话窗口外壳

- `services/SessionShellController`：两协议共用的**外壳逻辑**——窗口获取与焦点 / 全屏 / 可见性聚合、
  `RemoteCursor`、`ToolbarAutoHide`、输入接线、遮罩计时、快捷键拦截、surface 回调、重试 / 断开 / 关窗。
  **自身不含任何协议判断**。
- `services/SessionViewDelegate`：外壳与协议页之间的 host / hooks 契约（输入装配、标题、协议事件、
  surface、可见性、直达缩放、资源回收），协议差异由会话页以钩子注入。
- 会话页由 `SessionBackend.pageRoute()` 选择；协议页只保留**协议专属**的渲染与逻辑
  （RDP：剪贴板 / 文件传输 / 遥测 / 直达缩放；Moonlight：主机能力 / 关闭应用）。

## 5. 会话窗口模型与状态机（两协议共享）

- 每个会话在**独立的 `SessionAbility`** 主窗口中打开（`launchType: specified` + 唯一 `instanceKey`）。
  必须用独立 Ability 才能拿到完整标题栏（子窗没有最小化按钮）。
- 每次连接生成 `sessionKey`；待连接参数放 `SessionRequests`，**Want 里只带 key（密码不进 Want）**。
- **主窗口后台连接、成功后才开窗**；失败只报错、不开窗。每个会话独占一个会话后端（`SessionBackend`）。
- **关闭窗口即断连**：`SessionAbility.onWindowStageDestroy/onDestroy` → `SessionManager.release` +
  `SessionRequests.discard`。注意会话页 `aboutToDisappear` 在窗口关闭时**不触发**，别依赖它断连。
- **连接/断开由一个状态机统一管理**：`SessionManager` 是唯一权威，状态为
  `Idle → Connecting → Connected → Disconnecting → Idle`，失败走 `Failed`。会话窗口只是**视图**——
  它不创建也不销毁原生会话，只订阅该状态（渲染浮层）与原生 UI 事件；原生监听器由 manager 持有。
  列表与窗口因此不可能各持一份状态。
- **一次尝试 = 一个全新原生会话 + 一个 epoch**：`connect`/`retry` 都作废上一个 epoch，事件的 epoch
  不是当前值即被丢弃，所以被替换或已关闭会话的迟到事件不会驱动当前会话。
- **进行中的尝试拒绝新尝试**：**同一个连接**处于 `Connecting`/`Connected`/`Disconnecting` 时一律拒绝新尝试
  （不拆旧会话），一次尝试 = 一次完整协商 + 远端一次登录，另一端是**断开**（远端会话保留为已断开，不注销），
  所以连点"重试"或双击列表**不会变成连接/登录风暴**。`MIN_ATTEMPT_INTERVAL_MS` 只是次级保护
  （被拒绝的重试不返回成功，调用方不得据此切"正在连接"状态）；节流按 `connectionId` 记账、
  **在 entry 被清理后仍然保留**。RDP 侧的 native 终结细节见 [`rdp-session.md`](rdp-session.md)。
- **同一时刻只有一条连接在协商**：新尝试（`connect`/`retry`）先**取消其他仍处于 `Connecting` 的尝试**
  （停后端、发布 `Idle`，已开窗的随之关窗）再启动自己；`Connected` 的会话不取消（多会话并存仍合法）。
  依据：会话后端可能持有**进程级**原生状态（Moonlight 的 `g_active` / 进程级事件回调与 surface，见
  [`moonlight-native.md`](moonlight-native.md) §2），并发协商会互相覆盖，故取消须早于**新后端的构造**。
- **失败保留 entry 并只有一个终态事件**：连接失败（含超时）置 `Failed` 且不删 entry，窗口「重试」与
  列表再次点击据此发起新尝试；失败只产生错误终态（不另发断开），失败原因不会被"连接已断开"覆盖。
- **回到前台自动重连**（协议级设置，RDP `reconnectOnForeground` / Moonlight `mlReconnectOnForeground`，
  默认关）：会话窗口从后台回来时若发现会话已断，自动重连一次，省去手点「重试」。它**不是保活**，
  只兜"回来时已经断了"这一次：应用退到后台后，系统会先冻结网络资源（约 2s）、随后释放并 abort socket
  （约 12s），而官方对终端类会话（SSH/Mosh/SFTP）的判定是**不属于合法的高优先级后台服务**，普通三方应用
  无法用长时任务撑住这种"网络空载 / 低频心跳"（见 [`settings-and-storage.md`](settings-and-storage.md) §2）。
  两协议共用 `services/ForegroundReconnect`（每个会话页一个实例，构造即注册应用前后台监听），设置项在
  两协议 tab 的「会话」节各存一份（见 [`moonlight-connection.md`](moonlight-connection.md) §5）。
  - 触发条件是**真的离开过**（`onPageHide` 或 `onApplicationBackground` 置位），回来后**延迟一个宽限期**
    才判定：原生读线程要恢复运行之后才会把断开事件报上来，立刻判定会读到还没更新的状态。
  - 只接管 `Idle`（已断） / `Failed`。用户主动断开/返回不在此列；前台正常使用中掉线也仍走手动「重试」，
    不会被这里变成自动重连循环。
  - PC/2in1 上**窗口最小化不会驱动 UIAbility 进入后台**，因此这条路径在 PC 上通常根本不触发。

## 6. 窗口尺寸、全屏与可见性（共享）

- 默认尺寸按屏幕比例推导，屏幕查询失败则**不改窗口**；也可在设置里手动指定，或（会话）默认最大化。
  **尺寸只在窗口创建时生效**；`resize`/`moveWindowTo` 的单位是 **px**（不是 vp）。
- `sessionFullscreen` 经 `StartOptions.windowMode = WINDOW_MODE_FULLSCREEN` 直接全屏；
  会话窗的**系统最大化**在 `WindowController.setupSessionWindow` 里转成**沉浸式全屏**
  （隐藏标题栏 / dock 悬停）。
- **窗口不可见期间不应用 surface 尺寸变化**：最小化/完全遮挡时系统会重新分配它的 surface，恢复时先报过渡
  尺寸。故不可见期间只挂起，重新可见后再取一次 surface rect 应用；尺寸没变不下发。可见性口径只有两条，
  来自两种回调、可能只来其一：`windowVisibilityChange`（完全遮挡）与 `windowStatusChange` 的最小化，
  二者任一为"不可见"即算不可见。**部分遮挡返回 `true`**，"全屏会话被最大化窗口压住"判不到（系统不
  提供遮挡比例与被谁遮挡）。焦点、`windowEvent` 的 hidden/shown、页面生命周期都不是可见性判据（混写同一个
  状态只会互相覆盖），生命周期只承担"回来时是否已断"的重连记账。
- **RDP 可见性还会主动抑制服务端输出**（MS-RDPBCGR Client Suppress Output），见
  [`rdp-session.md`](rdp-session.md)；Moonlight 不做。

## 7. 主窗口关闭行为（共享）

**默认（多窗口，`autoHideMainWindow` 关）**：关闭主窗口时若仍有**会话窗口在运行**，先弹窗确认，确认后
**断开并关闭全部会话**（含尚在连接、还没有窗口的会话），再退出主窗口；无会话窗口则直接关闭、不弹窗。
判据是**已打开的会话窗口**（`SessionManager` 按 `sessionKey` 登记），不是会话条目（失败条目会保留、
连接中的会话尚无窗口）。拦截用 `UIAbility.onPrepareToTerminate`：**仅 2in1 生效**，需
`ohos.permission.PREPARE_APP_TERMINATE`；主窗口若注册 `windowStage.on('windowStageClose')` 该回调即失效。
主窗口最小化时先 `restore()` 再弹窗。

**自动隐藏主窗口（单窗口模式）**：全局设置 `autoHideMainWindow`（默认关）开启后仍**新建** `SessionAbility`，
但**销毁主 `EntryAbility`** 以真正隐藏（没有 hide API；`minimize()` 仍留在 Dock）。按**单会话**设计，故
`WindowController` 只用 `mainHidden` 布尔量、不跟踪会话集合。

- **进程内最后一个 UIAbility 被销毁 ⇒ 进程退出**：必须等会话窗加载完成后再 `terminateSelf()` 主窗口，
  否则会把整个应用杀掉。
- 关闭会话时**先拉起主窗口**，主窗口就绪后再终止会话；用 `windowStage.on('windowStageClose')` +
  `UIAbility.onPrepareToTerminate()` 拦截关闭（后者在部分设备上不触发），`closeSession` 去重 + 超时兜底。
- 关闭该选项则行为不变（多窗口可并存）。

## 8. 输入分流（两协议共享策略）

**分层**：输入端到端分三层，**UI 线程只做规范化与入队，不做协议工作、不做网络写**。
**L1（UI 线程）**是平台事件的翻译器：策略按关注点拆在协议中立的 `services/input/` 下——`PointerInput` 指针、
`KeyInput` 键盘、`TouchInput` 触屏、`TouchpadInput` 触控板、`WheelInput` 轴向滚动、`PinchInput` 捏合、
`GamepadInput` 手柄（见下），共用 `HeldInputs` 按住账本、`InputPolicy` 常量与共享指针几何；
`services/SessionInput.ets` 只做编排（瘦 Facade），出口是 `services/InputPort.ets` 的 `SessionInputPort`
（实现 `InputSink`）：动词变成入队，`relativePointerEnabled` / `hostDisplayScalePercent` 等查询由本地状态回答。
**L2** 是**唯一发送者**：一条专用 worker（`workers/input.worker.ets`），队列就是它的消息队列（FIFO、单消费者）；
两层的消息形状与投递类声明在 `services/InputProtocol.ets`（线程模型见 [`architecture.md`](architecture.md) §4.4）。
**L3** 是协议落地，在 L2 线程上构造，只实现"发一个事件"并回报**受理 / 拒收**：
`services/input/RdpInputSender.ets`（见 [`rdp-session.md`](rdp-session.md)）与
`services/input/MoonlightInputSender.ets`（见 [`moonlight-native.md`](moonlight-native.md)）；两个会话后端不再持有 sink。
会话页只把 ArkUI 事件转给 `SessionInput` 并注入配置（指针映射、指针空间、各项设置与能力），**不自持输入机制**。
**唯一的例外入口是轴向滚动**（见本节末）：滚动轴事件（触控板双指与鼠标滚轮，两个轴向）由 `SessionPicture`
里挂的 `Scroll2D`（`components/Scroll2D.ets`）的两个透明单轴 `Scroll` 叠层消费，它把逐帧增量交回
`SessionInput.handleEngineScroll`，policy 仍然只有一份。

**键表单表**：`utils/KeyMapper.ets` 用一张表同时给出 RDP 的 `scancode`+`extended` 与 Moonlight 的
`vk`+`extended`（及 lock 位），避免两协议键表漂移；`mapFull` 供 sink 取用，`map` 只回 `scancode`（RDP 侧兼容）。
**左右修饰键区分**：协议与主机都区分左右（`VK_LSHIFT/RSHIFT` 等）；表发左右专用 VK，修饰位按
**当前按下的修饰键集合**求值（不是按单个 VK 翻转），所以"按住左右 Shift 松一个"不会清掉 Shift 位。

**鼠标 → `onMouse`；真触屏 → `onTouch`；滚轮/触控板 → `onAxisEvent`。**

**输入来源识别（触控板 vs 鼠标）**：判据分两档，原因是触控板的两类事件上报方式不同：

- **触控板自己的手势带 `sourceTool === SourceTool.TOUCHPAD`**（双指滚动、捏合都是），这是设计上的判据，
  **常规分类直接用它**（滚轮 vs 触控板滚动）。
- **触控板的"点触"（tap）是系统合成的标准鼠标按键事件**（单指→左键、双指→右键），会**丢掉触控板身份**
  （`sourceTool === SourceTool.MOUSE`、`source === SourceType.Mouse`），与真鼠标不可区分。所以**要求确属
  触控板**的判断（触控板专属手势/参数，如双指双击→中键、触控板滚动速度）写成
  `sourceTool === TOUCHPAD || 事件来自触控板设备`，后者查事件自带的 **`deviceId`**：
  `inputDevice.getDeviceInfoSync(deviceId).sources` 是否含 `'touchpad'`（`@kit.InputKit` 的 `inputDevice`，
  无需权限；结果按 deviceId 缓存，`inputDevice.on('change')` 时清空——设备 ID 会因反复插拔/重启而变化）。
  `sourceTool` 先判，命中就不查设备，滚动这类高频事件因此不产生额外调用。

**"是不是真触屏"仍用** `source === SourceType.TouchScreen`（触屏与触控板在 `sourceType` 上是分开的）。

- **失焦即全量松开（防止远端卡键）**：焦点离开画面后，随后的 Up 投递给新的焦点目标，本会话收不到——
  按住的修饰键会永久卡在远端。触发点两个：窗口失焦（`on('windowEvent')` 的 `WINDOW_INACTIVE`/`WINDOW_HIDDEN`）
  与画面组件失焦（`XComponent.onBlur`）。客户端按**远端身份**记账仍按下的输入（按键=扫描码+扩展位 /
  Unicode 码点 / Win 替代键，鼠标=按钮标志），失焦时统一补发抬起并松开触屏与捏合；
  **新增输入通路必须进这份账**。
- **鼠标按键与移动分开**：移动与按键事件**分开送**，按键事件**不带移动标志**，否则远端会忽略点击。
- **连续位置流的投递按声明的窗口限量**：`onMouse` 的 Move 与触屏 Move 按设备轮询率到达，逐条转发 = 每条一个输入报文。
  controller 只把样本当普通动词交出去，**限量统一在 L1 闸门**（`services/InputPort.ets` 的 `SessionInputPort`）按声明表执行：
  窗口 >0 的流（RDP 指针 PDU，每个样本一条报文）取"只留最新值 + 窗口上限"；声明窗口为 0 的流
  （Moonlight 控制流按 1ms 合并鼠标样本）**同步直通**——客户端再限一次只会白加延迟与降采样。
  **离散事件（按下/抬起、触屏 Down/Up/Cancel）投递前，闸门先把挂起的窗口流冲出去**，否则点击落在旧坐标。
  窗口：鼠标 `MOUSE_MOVE_MIN_INTERVAL_MS`（约 125Hz）、触屏取「触屏-高刷新率」（8/20ms）。这是输入节流，不是显示帧率。
- **投递类与横切能力在流水线做一次**：每条流有一个在 `services/InputProtocol.ets` 声明的投递类——
  `ORDERED_RELIABLE`（入队即发、不可丢：首次按键与**任何**抬起、鼠标键、触点 down/up/cancel、锁定键同步、
  Win 替代键）、`LATEST_WINS`（窗口内只留最新值：连续位置流、手柄整份状态、触控板整帧）、
  `BOUNDED_REPEAT`（速率上限、超限**直丢不延后**：键盘重复按下，由按住账本判定`hasKey`）、
  `COALESCE_WINDOW`（窗口内累加后发一条：文本多码点合成一包，且只有具备整串报文的传输才聚合）。
  这条流水线的**唯一实现是 `SessionInputPort` 的闸门**：窗口、速率上限、丢弃计数、离散前的冲刷都只在那里出现，
  **不落在任何 controller 或 L3 sender 里**；触点 move 与 down/up 拆成 `TOUCH_MOVE` / `TOUCH_POINT` 两个 op，
  触控板整帧同理（全 move 走 `TOUCHPAD_FRAME`，带 down/up/cancel 走 `TOUCHPAD_POINT`），
  所以"down/up 永不进窗口、且总先冲掉本指挂起的 move"由声明直接成立。
  限量的目的是让**不可丢的那些事件排在队尾的时间不随输入洪水增长**，不是拿它们的延迟换秩序：
  首次按键、抬起、按钮、触点 down/up 一律不进窗口，`BOUNDED_REPEAT` 只丢不延后。
  丢弃按流计数，与 sink 拒收合起来即 `inputDropped`（协议侧不自己计数、不加专有遥测字段）；
  重复流的上限取在系统自动重复率之上，所以正常打字不会让它非 0。
  失焦补抬起、重新获焦补锚、穿透开关都走同一条队列，任一时刻的记账状态与远端一致。
- **相对指针模式（Moonlight 专有，只挂在「隐藏系统光标」档）**：`InputSink` 另一条相对通路
  （`pointerMoveRelative`/`setRelativePointer`/`relativePointerEnabled`，RDP 不支持）。开启后**不发绝对位置，改发相邻两次
  映射位置之差**（`services/input/RelativeUnits.ets` 的 1:1 口径：内容像素当主机计数 ⇒ 与绝对模式增益一致），
  **鼠标与触控板同一条逻辑**——触控板单指移动**不做**虚拟触点合成（把点击/拖动的语义也搬到触点通道上，
  两头都要额外适配，不值得）；位置差带本机指针曲线，所以**主机侧不要开鼠标加速**，否则叠成两层。
  增益还取决于**主机显示缩放**（相对注入按主机逻辑像素走，内容像素是物理像素）⇒ 按 `100 ÷ 主机缩放%` 除掉它，
  另有 `mlRelativeSpeed` 滑块做二次调整（`services/input/RelativeUnits.ets` 的 `relativeCountsGain` 是唯一换算点）。
  **主机缩放值**在连接时解析：RDP 取会话桌面的 `DesktopScaleFactor`（`RdpConnectOptions.scalePercent`，总有值）；
  Moonlight 取**基地版虚拟屏**的真实缩放（优先用本次设置成功的值，`不额外设置`／设置失败时才读 `/displays` 的当前值）。
  这条读取**只有基地版有**，且只在串流的就是那台虚拟屏时成立——主机没启用虚拟屏、或非基地版（原版 Sunshine / GFE）
  都拿不到 ⇒ 按 100%（不校正）。该值随会话配置交给 L1（`SessionInputPort.hostDisplayScalePercent`，L1 侧的
  换算点直接读它，不跨线程），并在连接成功后由会话页重新申明一次——输入可以在连接结果解析之前装配。
  连接后先发一次当前位置的绝对坐标作**锚点**（`SessionInput.reportPointerPosition()`），此后**每次重新获焦**都重取基准并
  补发一次锚点（`reanchorPointer()`，见 §4 的窗口焦点回调）——主机光标可能在失焦期间被别处使用而离位。
- **为什么不用 rawDelta**：`MouseEvent.rawDeltaX/rawDeltaY` 是**未加速**的设备位移（MMI 侧取自 libinput 的
  `*_dx_unaccelerated()`），本该是最合适的一路；但 MMI 对 **USB 真鼠标**按 **800µs 门限做回报率过滤**：间隔不足的
  motion 事件被**整条丢弃**，其位移只累加进**位置**、**不写回 raw**（实现分处 MMI 的 `service/mouse_event_normalize`
  与 `service/event_handler`：前者把 `filterX/filterY` 挪到下一次事件的 offset，后者在门限内直接 return）⇒
  高回报率鼠标只能拿到一部分，相对模式**按回报率成比例变慢**。门限是编译期开关、无系统设置可关，丢帧数应用侧也
  拿不到（`addLocalInputEventMonitor` 的 mask 无 move，C 拦截器在其下游且只有加速后的坐标）。位置是**累计补全**的，
  所以改用位置差。
- **相对模式的边界**：系统无 pointer lock，**本机光标仍会移动并在屏幕边缘被夹住** ⇒ 该方向位置差为 0、指针停滞
  （"指针在哪一格"的判定同样失真）。消掉要**写系统光标位置**（`inputEventClient.createMouseController().moveTo()` /
  `OH_Input_InjectMouseEvent`，需 `ohos.permission.CONTROL_DEVICE`）；**读**位置用 `OH_Input_GetPointerLocation`
  （聚焦应用可调）。
- **触屏**（只认 `event.source === SourceType.TouchScreen`；手指 id `+1` 避开 0 空槽）：
  - `handleMouse` 要过滤同源兼容鼠标事件，避免"一触两通路"产生杂点击；
  - 重复 `TouchType.Down` 忽略（补发 UP+DOWN 会变成"松开+重按"的假点击）；
  - `TouchType.Cancel` **不代表抬指**（move 会被打断）：挂起不抬指，若附近随后出现新的 Down/Move
    判定为同指续接触、只发 MOTION，超过 `CANCEL_HOLD_MS` 无续接触才真正 UP；
  - 远端的长按右键菜单依赖**触点状态序列合法**：触点的 Move 由 L1 闸门按窗口合帧、Down/Up
    立即发（离散事件先冲掉本指挂起的 Move），一个触点的**首条上报因此必须是 DOWN**；首条若成了 UPDATE/UP，远端不会 engage 它——长按在远端
    成不了 press-and-hold，还会留下一个不消失的触点视觉。应用侧只保证 Down/Up 成对、**不给同一手指重复下发
    DOWN**；合并后的合法性由协议侧兜住（RDP 见 [`rdp-session.md`](rdp-session.md)）；
  - 压力按透传（`[0,65535)` → 归一化，0 表示设备未上报）。
  - **触点可被外层持有再回放**：会话外壳的触屏下拉唤出判定器（§9）能从 `Down` 起扣住一个触点，判定后经
    `TouchInput` 的逐点入口按原序回放，因此这条通路必须能以"一个纯样本"（相位 + 手指 id + 坐标 + 压力）
    入账，而不只接受 `TouchEvent`。
- **轴向滚动（触控板双指 + 鼠标滚轮，两个轴向）：滚轮默认全部交给系统滚动引擎；触控板双指在主机支持专用通道时改走触点通道**：
  `SessionPicture` 挂的 `Scroll2D`（`components/Scroll2D.ets`）在 XComponent
  上叠**两个透明的单轴原生 `Scroll`（一竖一横）**，跟手、加速、**惯性抛滑与打断**全由系统负责（ArkUI 的滚动惯性
  只在 `List`/`Grid`/`Scroll`/`WaterFlow` 内部的 `Scrollable` 私有 tick 里：速度估计、摩擦动画、连续拖拽增益、打断，
  借官方组件才拿得到完整滚动物理）；我们只看 `onWillScroll` 的逐帧偏移（**vp**，正是 mapper 口径），按
  `120 / (REMOTE_PX_PER_DETENT × SCROLL_CALIBRATION) × pxPerVp × 速度倍率` 折成轮转量交给 sink
  （RDP 直接缩放中则落到本机画面平移，见下）。三条输入（直译触控板 / 直译滚轮 / 引擎）共用这一条口径，
  **改它 = 等比改"远端滚动总幅度"**（方向、惯性曲线、发射粒度、捏合、按键各自独立）。
  - **引擎归属按来源分流**：`consumesAxis` 只吞引擎负责的轴事件（鼠标滚轮恒归引擎；触控板双指在专用通道接管时
    不吞，由 `SessionInput` 走模拟两指滑动），被接管手势的引擎帧由 `engineAcceptsFrame` 丢弃，避免一份手势两份滚动。
  - **触控板滚动的三种落点**：①专用触控板通道（默认，仅基地版）→ 第二层物理是 Windows 原生触控板；②系统滚动组件 +
    滚轮（设置强制或主机无该能力位）→ 第二层是 ArkUI 引擎，最跟手且有刹车；③直译 + 滚轮（全局「回退直译映射」）→
    无惯性。三者都只保留本机这一层换算（**轴值与捏合值实测未见随速度的增益**，按位移线性使用），区别只在第二层是谁。
  - **必须是一轴一个组件**（不能用单件 `ScrollDirection.FREE` 的 2D 版）：滑行的"打断"由单轴滚动件自己做——
    **任何新的触控板手势一开始就把正在跑的滑行停掉**（`Scrollable::HandleDragStart` 对 `SourceTool::TOUCHPAD`
    调 `HandleTouchDown`，后者明确 "If animation still runs, first stop it"），2D free 不停滑行（它只在收到真正的
    touch Down 时停，触控板两指按住那条路给不到应用），而框架内部状态无从从应用侧干预。
  - **两轴用并行桥而不是互斥裁判**：斜向手势要两个轴一起消费，而不同节点的识别器默认互斥。两个叠层各自
    `.shouldBuiltInRecognizerParallelWith(...)`（文档「手势拦截增强」，同一套机制也用于嵌套滚动），在响应链里
    按 `.id()` 认出对方的 `PAN_GESTURE` 并返回它即建并行桥（认不到就返回 `current` 自身，等于不建桥——
    返回 `undefined` 不在类型里）。**轴事件也走这条**：`EventManager::TouchTest(AxisEvent)` 会用合成的 TouchEvent
    跑完整的触摸测试，所以桥对触控板/滚轮手势同样成立。
  - **只服务触控板滚动与鼠标滚轮**：`handleTouch` 给**直接触摸接触**记时（`TouchType.Down`，`touchEnabled` 关着
    也记），接触落在某次手势之后即整体丢弃其帧——叠层自身仍会被手指拖（偏移钉住，无可见效果），但**一个字节
    都不会进远端**，触屏只有自己的路径。**不要在命中测试上拦**：`onTouchIntercept` 返回 `HitTestMode.None` 会把
    节点自身的命中模式一起改掉，轴事件的 `AxisTest` 随后也跳过它，滚动整条失效；`HitTestMode.Transparent` 保证
    鼠标 / 键盘 / 焦点仍归 XComponent（鼠标左键拖动留给远端拖选）。
  - **位置与来源都取原始轴事件**：引擎的帧不带位置，来源也不另立一套——轴事件先记账（`noteAxisEvent`：指针位置 +
    触控板/滚轮分类，后者决定吃不吃「滚动速度」）。单轴件的 `scrollSource` 本可给出同样的分流（触控板是
    `DRAG`/`FLING`、滚轮是 `OTHER_USER_INPUT`，最初的单轴实现正是这么写的），两条路都通，实现只留一条。
  - **两轴各自成对、与同名 `axis*` 分量同号**（实机确认），喂 mapper 按 `(vertical, horizontal)` 落位——**这一层
    最容易串轴**。**Shift 的横向语义有两个执行者，别弄混**：`ConvertToOffset` 的 Shift 改写**只在鼠标分支**，
    所以 **Shift+滚轮**由框架改写成横轴（实测转横滚；回退路径不改写，按纵向轮 + Shift 修饰键发出）；
    **触控板不吃这个改写**，"Shift+触控板竖滑 → 横滚"是**远端应用**按 Windows 约定自己做的（我们只发纵向轮 +
    Shift 键态）。直接缩放中的平移是本机的、没有这个约定，所以竖滑就是竖移（Shift+滚轮则仍是框架改写的横轴，
    表现为横移）——这是两种模式的固有差异，不是漏改。**未经 Shift 的鼠标滚轮要留一分怀疑**：框架把它换算成
    `Offset(-verticalAxis, -verticalAxis)`（纵向值同时进了两个分量），单轴件按方向过滤只会清掉"非本轴"的那一半，
    所以竖直滚轮可能让**水平件**也出一帧假横滚（是否复现取决于设备侧的"异向手势"标志，待实测）。
  - **同一手势的两轴是否都生效，由接收端应用决定，不是客户端能保证的**：客户端把有位移的轴各自成帧、两轴都交给
    sink（RDP 为 `WHEEL`/`HWHEEL` 两条独立 PDU，见 [`rdp-session.md`](rdp-session.md)），但目标应用对同一次手势内
    换轴 / 两轴并发的处理并不一致——有的按先出现的方向**锁轴**（本次手势内另一轴的 wheel 被丢弃），有的两轴都收；
    `Shift` 是否被当横轴也随应用而异（见上）。**不能用某个应用的行为判定客户端有没有丢轴**，也不能只凭日志下结论；
    判据是同一手势、同一链路下**换应用对照**的端到端实测。本机（鸿蒙侧）应用同样存在这种不一致。
  - **偏移钉住**（两个叠层的 `onWillScroll` 都返回 `{xOffset:0, yOffset:0}`，假内容静止），**各自**在主轴方向比视口大
    `2 × ENGINE_FRAME_HEADROOM_VP` 以保证可滚范围非退化；`scrollBar(Off)` 显式关（默认 Auto 会画滚动条）。
  - **逐帧判定，不做手势级锁定**：`consumesAxis` 吞掉**引擎负责**的滚动轴事件（两协议共用），例外是**捏合**（归
    `SessionInput`）与**专用触控板通道接管的手势**。增量由 `engineAcceptsFrame` 逐帧判归属（直接触摸接触之后的、
    捏合中的、被通道接管的触控板手势的帧都不归引擎），
    会话页再逐帧定去向（RDP 直接缩放 `viewScale > 1` 且非捏合中 → `panView`，同一套引擎物理；否则远端）。
    **判据只许用"下一次输入就会刷新"的状态**（触屏时间戳、捏合标志、来源分类）：框架何时算一次手势结束
    并不总是可观测，跨手势状态会永久卡住。代价是中途才出现的捏合、缩放到 1.0 后的惯性尾巴按逐帧语义可能漏
    （都要重新接触手指，框架在接触时会先掐断惯性）。
  - **回退＝直译**（开发者选项，全局 `scrollEngineFallback`，默认关＝引擎开）：同一个 mapper 直接累加 vp 位移、
    **无惯性**，供逐齿精确滚动的特殊操作；口径与窗口/画面尺寸无关（缩小窗口手感不变），远端行高/每齿行数原样
    透传，「滚动速度」只缩放轮转量、发射粒度不变。
  - **轴值单位**：滚轮是**角度**（1 度 → `LINE_HEIGHT_DESKTOP/MOUSE_WHEEL_DEGREES` = 21/15 vp；`event_constants.h`
    给 `MOUSE_WHEEL_DEGREES = 15` / `LINE_HEIGHT_DESKTOP = 21vp`，本机再 `× WHEEL_VP_PER_DEGREE` 落同一口径），
    本机每齿行数（MMI `rows × libinput`）已折在轴值里，`scrollStep` 冗余、本机调快则远端同步变快，**亚齿/多齿
    都按增量发**（保留高分辨率；主机是否真保留亚齿取决于其注入通路，见
    [`moonlight-native.md`](moonlight-native.md) §2.2），滚轮用独立 mapper 保持原速；触控板是**位移像素**
    （`sourceTool === TOUCHPAD`；**轴值与捏合值实测未见随速度的增益**，按位移线性换算），直译路径先 `÷ pxPerVp` 统一到 vp。
  - **触点通道的量纲**：轴位移 px × 平移比例 → 声明板面的毫米行程。比例按**每齿毫米**给（`TOUCHPAD_PAN_MM_PER_AXIS_PX`
    = `TOUCHPAD_PAN_MM_PER_DETENT` ÷ `REMOTE_PX_PER_DETENT × SCROLL_CALIBRATION`），实测约 **4.8 mm 双指位移 / 远端一齿**：
    毫米 → 滚动这一段的增益在 Windows PT 侧（随主机 PT 与滚动设置、屏幕缩放变化），不可由本仓推导，只能按此口径实测；
    声明板面尺寸（`TOUCHPAD_WIDTH_MM/HEIGHT_MM`）宿主会原样采用（`clamp` 到 40–200mm），中间无额外汇率。方向属真机判据。
  - **两个量分开**：`REMOTE_PX_PER_DETENT`（=48）是**名义锚点**（远端每齿标称内容量：`3 行 × 16px`）；
    `SCROLL_CALIBRATION`（=4）是**端到端标定倍数**（实测定值）。分开后名义常数保持字面含义、标定倍数独立可调；
    有效"每齿内容量" = 48 × 4 = 192。`pxPerVp` 只做本机 px↔vp 折算（触控板 `÷`，引擎与滚轮按原值进入）。
- **触控板捏合**（设置「触控板两指捏合模式」，三选一）：
  - **Ctrl+滚轮**（默认）：按 `1/(20×速度)` 线性映射为 **Ctrl+滚轮**（「缩放速度」），由远端应用自己缩放。
  - **触摸模拟**：在鼠标位置**合成两个原生触点**做真实双指缩放——距离 1:1、初始间距取窗口较短边的百分比
    （默认 8%，不写死分辨率）、按设置角度斜置。**不设计时器**，只在真实 `AxisAction.END` 结束。
    - 起手 **30ms 内不发 MOTION**：避免协议侧合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时捏合会失效）。
    - ArkUI `AxisType` **没有旋转轴**，拿不到真实手指朝向（多指不上报手指信息），所以用角度设置顶替。
    - **捏合期间给一个定位光标**：合成了真实触点后，系统会把鼠标指针隐藏（触屏活动态），此时若开着
      远端光标就完全看不到捏合锚点。于是捏合期间（`RemoteCursor` 的捏合覆盖）把系统光标换成鸿蒙原生
      **手势手型**并强制可见：**张开手**（`HAND_OPEN`）= 手指张开/放大，**抓取手**（`HAND_GRABBING`）
      = 手指收拢/缩小；方向按手势比例判、带 2% 死区避免抖动来回切，只在方向真的翻转时才改一次样式
      （`setPointerStyleSync` 是系统调用，不能每帧调）。结束时恢复远端位图/隐藏态。
  - **直接缩放**（RDP 呈现器专有，Moonlight 不暴露）：见 [`rdp-session.md`](rdp-session.md)。
- **触控板双指双击＝中键**（全局设置，默认关，判定器 `services/TouchpadTapFilter.ets`）：触控板对应用只上报
  单指点触＝左键、双指点触＝右键、双指滑动/捏合＝轴事件，**多指与指关节都不上报**，所以中键只能自己合成。
  开启后把双指轻触的**右键**延迟一个双击窗口发出：窗口**从第一次轻触的按下起算**，窗口内出现第二次双指轻触就
  改发**中键**（单次仍发右键），因此单次右键有约 300ms 判定延迟；关闭则无延迟直发。
  - **轻触与实体右键的区分（避免按住拖动被吞）**：ArkUI `onMouse` 不带「双指轻触 / 按压右键区域」语义——
    `SourceType` 无触控板档（触控板报 `Mouse`）、`SourceTool` 只有 `MOUSE`/`TOUCHPAD` 两档、`pointer.RightClickType`
    是系统配置项而非逐事件字段——故只以**按下→抬起时长**区分：按下后超过 `TOUCHPAD_PRESS_HOLD_MS`（远大于系统的
    轻触按下-抬起间隔）仍未抬起、或期间出现移动，即判为实体右键，立即补发按下、其抬起直发，拖动不受判定窗口影响；
    双指轻触的按下-抬起近乎同时，落在窗口内参与单/双击判定。
  - **归属与边界**：判定器只吃触控板来源的右键**按下/抬起**（`MouseAction.CANCEL` 视作取消），移动事件只作观察、
    **永不吞掉**（拖动中的移动也可能带 `button === Right`，按 button 拦截会让拖动期间位置不再更新），随后照常走
    移动通路；状态与定时器自持有，`releaseAll`/`dispose`/关闭设置统一走 `reset()`。合成点击一律按**一次按下＋抬起**
    整发（不会卡键）。真鼠标右键不受影响。
- **自定义 Win 键映射**（全局设置，0＝关闭）：在 `SessionInput.handleKey` 里把指定 `keyCode` 改发 Meta。
  **真实 Win 键不转发**，避免"远端+本机"双重映射。
- **快捷键穿透**（全局设置 `shortcutPassthrough`，默认关）：系统级按键拦截
  （`OH_Input_AddKeyEventInterceptor`，需 ACL `ohos.permission.INTERCEPT_INPUT_EVENT`）接管键盘，
  让 Win / Alt+Tab / Esc 这些本会被 shell 吃掉的键也到远端。拦截事件按**本会话**路由到
  `SessionInput.handleInterceptedKey`（映射与记账同 `handleKey`，但**不跳过真实 Win**）；
  拦截期间 `handleKey` 不参与，避免双发。注册经 `SessionBackend.setShortcutPassthrough()`，**归属按获焦窗口**：
  进程级单拦截器，只有获焦会话持有，切窗/失活即释放。（与"自定义 Win 键映射"不同，穿透下**真实 Win 键会被转发**。）
  - 拦截是**进程级、且只在应用获焦时投递**，故只在**本会话窗口活动**且会话 `Connected` 时注册；其他窗口
    （设置页 / 主列表）变为活动、或本页离开即释放，否则那些窗口的键盘也会被吞掉。
  - 代价是开启期间**本机的系统快捷键一并被远端接管**，所以默认关；注册失败（缺 ACL 等）时后端返回
    `false`，UI 保持普通按键通路而不是把键盘丢掉。
- **手柄（控制器，Moonlight 专有）**：OH 把柄键上报为**按键事件**、把摇杆与十字键上报为**焦点轴事件**
  （`FocusAxisEvent`，只投给**获焦组件**——画面 XComponent 就是获焦者）。策略在 `services/input/GamepadInput.ets`，
  只会话页按 Moonlight 装配（**RDP 不注入**，其按键/方向键路径不变）。
  - **按键按 evdev 语义映射，不按型号打补丁**：平台的键表是**固定的 evdev→KeyCode 表**（`key_map_manager`），
    所以同一个码在任何手柄上都同义，客户端只需一张语义表：标准键位（A/B/X/Y、LB/RB、Start/Select、摇杆按、
    Guide）＋**十字键中键→A**、**`MENU`→Start**、**`BACK`→Select**（这三个键盘上也有，所以**只取 `keySource`
    是手柄的来源**，键盘不受影响）＋**十字键的另一路**（`BTN_DPAD_UP..RIGHT`，按离散按键报十字键的手柄）。
    OH 对表里没有的 evdev 码上报 `10000+码`。
  - **协议里没有对应位、或主机不消费的键一律不发**（消费掉即可）：
    - 分享/截屏键（evdev `KEY_RECORD`(167)、`BTN_MISC`）：协议唯一能放的 `MISC` 位**被主机并进 Guide**
      （X360 路径 `HOME|MISC → XUSB_GAMEPAD_GUIDE`），等于把**错的**按钮送出去 —— 宁可不发；
    - 背拨片（`BTN_TRIGGER_HAPPY5..8`）：`PADDLE1..4` 两个主机后端都不消费，发了也是空转；
    - 认不出的其它柄键：给未知按钮猜一个桶只会把错的按钮送到主机，比没有更糟。
    这些码仍然算"手柄键"（被消费），只是不产生任何位——否则会漏进键盘通路。
  - **来源判据**：`event.keySource === KeySource.JOYSTICK`，外加平台只可能由手柄产生的码段（2301–2315 /
    2401–2416 / 3100–3109 / `10000+` 的 167、544–547、708–711）兜住未标来源的设备；在 `SessionInput.handleKey`
    里**抢在键表之前消费**——十字键的码在键表里是方向键，否则会被当成箭头键再发一遍。
  - **十字键两路择一**：十字键有**按键**与 **hat 轴**（`ABS_HAT0X/HAT0Y`，原值 -1/0/1）两种上报。**hat 真动过一次**
    之后方向位就归 hat（同一方向的按键事件被忽略——否则按键抬起会把 hat 按住的方向也清掉），此前按键独立生效。
  - **方向键不再走本机焦点**：方向键/十字键按键默认会带动**本机走焦**，一走焦后续轴事件就投给别的组件了；
    `SessionPicture` 用 `focusScopeId(id, true, false)`（`arrowStepOut=false`）把焦点圈在画面内，Tab 不受影响。
  - **最短按下时间**：一次轻点到达时只是相隔几毫秒的 down/up 对，而主机按自己的节奏采样手柄、游戏再按帧采样，
    过短的按下会**整个丢失**。所以**释放被延后到至少 25ms 的按下时长**（与安卓客户端同一处置）；只在真的过短时
    才走定时器，正常操作不引入延迟。
  - **轴来源：平台的 Game Controller Kit 是首选**（官方 C 接口，`libohgame_controller.z.so`，原生薄封装
    `core/hmrdp_gamepad_kit.*` + N-API `gamepadKitSetListener/Stop/Devices`）。它的轴事件**带来源**
    （`DPAD / LEFT_THUMBSTICK / RIGHT_THUMBSTICK / LEFT_TRIGGER / RIGHT_TRIGGER`），官方指南给了对应表：
    **左摇杆 = X/Y、右摇杆 = Z/RZ、十字键 = HatX/HatY、左扳机 = Brake、右扳机 = Gas**。于是"哪对轴是右摇杆、
    哪对是扳机"**不需要任何推断**，`RX/RY` 也不在官方手柄模型里（Kit 没有它们的取值接口）。
  - **兜底路径 = 同一张官方表**：Kit 注册失败（设备没有该服务 / 服务拒绝）时，改用焦点轴事件，按上表读。
    两路都**只看官方表**：不按 `RX/RY` 在不在切布局，也不靠观察去猜角色。
  - **两路要合成同一个控制器槽，只有"设备身份"需要对齐**：Kit 用**字符串 key**标识设备，而键事件带的是
    multimodal 的数字 deviceId。绑定顺序：key 解析成数字且确是手柄就用它 → 否则按**设备名**匹配 → 再否则
    **只有一个手柄时**就是它；都对不上时该手柄另占一槽（并记一行日志）。这是整套里唯一一处"对齐"，与轴角色无关。
  - **扳机两种量纲折成 0..1**：Kit 文档标的是 [-1,1]（可能负端静止），multimodal 的 `GAS/BRAKE` 是 [0,1]；
    判据取"该轴出现过负值"（`(v+1)/2` vs 原值），只影响量纲、不影响角色。只报按键的手柄仍用 `L2/R2` 兜底成 0/255。
  - **设备与掩码**：在线手柄列表取 Kit 的 `gamepadKitDevices()`（比按 `sources` 含 `joystick` 的猜测准）；
    Kit 不可用时才退回该猜测。插拔仍由 `inputDevice.on('change')` 驱动槽位与掩码。
  - **十字键两路择一**：十字键有**按键**与 **hat 轴**（`HAT0X/HAT0Y`，原值 -1/0/1）两种上报，Kit 的 `DPAD` 轴
    也算轴那一侧。**轴真动过一次**之后方向位就归轴（同一方向的按键事件被忽略——否则按键抬起会把轴按住的方向
    也清掉），此前按键独立生效。
  - **方向键不再走本机焦点**：方向键/十字键按键默认会带动**本机走焦**，一走焦后续轴事件就投给别的组件了；
    `SessionPicture` 用 `focusScopeId(id, true, false)`（`arrowStepOut=false`）把焦点圈在画面内，Tab 不受影响。
  - **最短按下时间**：一次轻点到达时只是相隔几毫秒的 down/up 对，而主机按自己的节奏采样手柄、游戏再按帧采样，
    过短的按下会**整个丢失**。所以**释放被延后到至少 25ms 的按下时长**（与安卓客户端同一处置）；只在真的过短时
    才走定时器，正常操作不引入延迟。
  - **口径**：摇杆 [-1, 1]、扳机折 [0, 1] 后再 ×255、摇杆 **Y 取负**（线序上正=上，与参考客户端一致，真机若
    上下颠倒就同步翻转两轴）。**不做死区**：归一化已按设备量程取值、摇杆静止即 0，剩下一层死区（XInput 常量 /
    游戏自己的阈值）交给主机与游戏（手游端那套 0.13 扳机死区是配合它自己的量程判定的）。
  - **记账**：按住状态记在 `GamepadInput` 自己的槽位表里，`releaseAll()`（画面失焦 / 窗口失活 / 释放）把每个槽位
    归零补发并清掉待发释放，与键盘鼠标同一触发点。
  - **热插拔**：`inputDevice.on('change')` 的 add 给一个槽位并发一次**到达声明**，remove 把该槽位状态清零、掩码去掉
    该位（主机据此回收虚拟手柄）；槽位上限 4，掩码随每次状态事件一起发。协议与 `/launch` 掩码见
    [`moonlight-native.md`](moonlight-native.md) §2.5。
  - **不做（对照安卓客户端的结论）**：缺键的组合模拟（Start+LB→Select、Start+Select→Guide、退出的四键组合）是
    它替代"没有工具栏"的界面手段，本机有工具栏且 OH 手柄本身能报 Select/Start；多设备手柄融合（安卓按 descriptor
    把同一手柄的多个节点并到一个控制器号，OH 已知手柄都是单设备）；把没有 hat 的手柄那组 `BTN_TRIGGER_HAPPY1..4`
    当十字键（安卓自己也注明"无法确认"——认错的代价是把面键变方向键）。
  - **诊断**：手柄挂载时打**一行**（含解析到的设备与槽位），Kit 与键事件的设备身份对不上时也记一行；这只是留痕，
    **不是适配手段**——适配由官方 Kit / 官方轴表完成，不需要用户进开发者模式配合。
- **锁定键（Caps / Num / Scroll Lock）路径**：读本机锁定态并与已下发值比对后同步远端；RDP 的 PDU 落地见
  [`rdp-session.md`](rdp-session.md)，Moonlight 由主机自行维护。
- **捏合与触控板的协议归属**：捏合（各三档）、触控板双指滚动（直译模式，以及引擎模式的增量落地）、
  双指双击→中键都在 `SessionInput`，**两协议共享**；各协议的落地（RDP 的 RDPEI/直达缩放，Moonlight 的
  `LiSendTouchEvent`/原生 precision touchpad）见对应协议文档。

## 9. 工具栏（显示与自动隐藏，共享）

- **显示规则**：全屏模式沿用**悬浮自动隐藏**（鼠标靠近屏幕顶部才滑出；触发/隐藏延迟在
  **开发者选项**里调）；窗口模式**始终显示**，且作为普通行布局在远程画面**上方**（渲染区自然扣除工具栏高度、
  不被覆盖，指针映射仍以 XComponent 局部坐标为准）。开发者选项开启时，遥测另起**第二行**，主行布局与
  非开发者模式**完全一致**（工具栏变高，`toolbarHeight` 随之增长，全屏自动隐藏的判定边界跟着用这个值）。
  遥测行由共享的 `TelemetryRow`（`components/SessionChrome`）渲染，页面只产出协议专属的 `TelemetryField[]`；
RDP 主行的分辨率 / 超分指示与遥测字段见 [`rdp-session.md`](rdp-session.md) §5，Moonlight 见
   [`moonlight-session.md`](moonlight-session.md) §2。
- **自动隐藏的判定口径**：全屏下只看**指针在哪一格**——顶部触发带（贴顶一条**居中**窄带，停留 `hoverDelay` 才
  滑出）、工具栏自身、其余画面；**指针在工具栏上 ⇒ 保持显示，且不允许任何隐藏定时器在跑**，在画面里 ⇒
  起 `hideDelay` 收起。指针格由**画面顶部居中的专用窄条**（`SessionPicture` 的 reveal 条：只在全屏且工具栏
  隐藏时存在，宽度占画面宽度的百分比——`revealBandWidth`，两端留空不参与检测以免贴边 / 角落误触，
  悬停上报 `TOOLBAR_ZONE_TRIGGER`、移出上报 `NONE`；它是**传感器**，透明命中、不遮挡画面，
  否则那一条带内的滚动与悬停都会失效）与**工具栏自身**（悬停上报 `TOOLBAR_ZONE_BAR`）
  共同上报同一格（`TOOLBAR_ZONE_*`）：全屏下工具栏盖住二者，指针在工具栏上时都收不到事件。所有触发
  （指针移动 / 悬停、传输起止、全屏切换、会话状态变化）只重新求值这一次判定，不各自管定时器，
  过期定时器因此不会把工具栏从指针底下收走。
- **触发带有进度提示**：指针在顶部触发带停留期间，屏幕顶边有一条**随停留进度填充的细线**（时长与
  `hoverDelay` 一致，因此填充完成与工具栏滑出同时发生；指针一离开触发带就回退消失），预告工具栏
  即将出现。它只是提示，不参与上面的判定：指针事件必须能穿过它到达画面与工具栏。
- **触屏唤出（无指针时的唯一入口）**：悬停档在纯触屏设备上永远不会触发，所以工具栏改为**顶部边缘下拉两次**：
  第一次下拉贴顶升起一条**抓取横条**（`ToolbarTouchBar`：与 `ToolbarRevealHint` **同槽位、同宽、同一套
  由 fill 经 `animateTo` 从中心展开的动效**，只比它厚，无底板——提示线与手柄因此只差"厚度"这一件事），
  窗口 `TOUCH_HINT_HOLD_MS` 内的
  第二次下拉唤出工具栏；窗口内没有第二次则横条自行淡出、手势重新从"第一次"算起。**工具栏已显示时反向收口**：
  画面上的一次触摸（点击或拖动）即收起工具栏。
  - **横条只做手柄，教学文案走系统提示**（页面的 toast，首次下拉时一次性弹出）：文字挂在横条上会变成
    一块漂浮的牌子，与系统手柄的观感不符。
  - **判据只用"下一次输入就会刷新"的状态**：只认 `SourceType.TouchScreen`、从窗口顶部触发带
    （`REVEAL_BAND_VP`，与鼠标的顶部带同义但按触摸尺度取值）起手、以下滑为主（横向越限即放弃）的
    **单指**触点；多指加入的整个手势交回触摸通路。
  - **归属靠持有触点，不靠命中测试**：手势从 `Down` 起扣住该触点——判定为唤出/收起就**整段丢弃**
    （远端看不到这次拖动），否则把扣住的样本**按原序回放**给触摸通路。因此顶部区域既不是远端触摸的
    死区，也不会在下拉唤出时向远端泄露一次拖动。回放走 `TouchInput` 的逐点入口，`TouchEvent` 不跨回调持有。
  - **只在本协议真的把触摸转给远端时启用**（`SessionInput.touchForwarded()`）：否则触摸会被合成鼠标事件
    （`SourceType` 仍是触屏）走另一条通路，扣住的触点收不回来。
  - 扣住触点期间仍要通知滚动引擎"有直接触摸接触"（`noteTouchContact`），否则 `Scroll2D` 的帧会被当成
    滚轮滚出去。

## 10. 远端光标安装器（共享）

- RDP 与 Moonlight 共用同一安装器 `services/RemoteCursor.ets`：原生只发**解码后**的 BGRA + 热点 + 内容键 +
  桌面倍率，策略全在 ArkTS。（**模拟器无鼠标，只能真机验证**。）
- **分层**：必须原生的只有两样——协议解码（RDP 的 xor/and 掩码；Moonlight 库直接给 BGRA）与重采样数值核
  （`core/hmrdp_bitmap`：预乘 alpha 面积平均缩小 / Lanczos3 + 对比自适应锐化放大，经 NAPI `scaleBgraBitmap`
  **句柄进句柄出**，位图不为变换本身穿越边界）。其余全是 ArkTS 策略：目标尺寸（源 × 倍率，按比例钳 ≤256
  系统上限）、热点换算、PixelMap 构建与安装、捏合定位光标、无鼠标设备的能力探测、可见性与默认形状状态机。
  安装器**全权消费句柄**：任何分支都会 `bufferRead`/`bufferRelease`/转交缩放，句柄不可能泄漏。
- **状态机不变式**：系统光标始终等于「最近一次送达的（形状，可见性）」。形状事件**蕴含显示**——
  RDP 没有独立的「显示」事件（只有 `SetNull` 隐藏），形状或默认箭头到达即是显示；Moonlight 的 `visible`
  标志可随形状同发，以其为准（改形状但保持隐藏是合法组合）。`Default` 清形状回默认箭头。
  - **重放点**：正确性**不依赖协议重发**（RDP 每次指针更新都重发形状，Moonlight 只在 `shape_id`
    变化时发一次），而由安装器在「就绪 / 可能失效」的时机主动重放当前状态。失效时机两个且
    **互不包含、时序不定**：窗口激活与指针重进窗口（两个会话页的 `onHover` 都调 `reapply`）。
    窗口绑定本身也是重放点（`setWindow` 里即重放——窗口是异步获取的，首个形状事件可能先到，状态先装着）。
    任何新增的消费方或失效场景都应挂到重放上。
  - **会话终态是兜底的收尾点**：连接断开（远端断开同样算）或失败后**窗口仍留着**（只弹浮层、重试 / 返回还在），
    此时既没有窗口失活事件，远端也不会再重发来纠正隐藏。故安装器在终态**停止消费远端事件**（迟到的隐藏不得再
    压住本机光标）并**无条件恢复进程级可见**——`apply()` 在缺窗口 / 窗口 id 非法时会早退，单次重放不足以保证；
    同一窗口重试（`Connected`）时再显式恢复消费。
  - **进程级可见性只有一个、按活动窗口唯一归属**：形状**按窗口**安装，但显示/隐藏是**进程级**的。
    安装器把它**按「活动窗口」唯一归属**（静态 owner），非活动窗口只保留自己的状态、不碰系统开关；
    窗口失活 / 隐藏 / 销毁时**交还**开关并恢复显示——若开关已被新活动窗口抢先认领则不动。
    没有这层归属，一个把光标隐藏的会话切走后会把主窗口的光标也留在隐藏态；会话窗口关闭**不跑页面 teardown**，
    `windowEvent` 这条失活路径才是可靠的收尾点。
  - **会话间状态独立**：每个会话页持有自己的安装器实例（形状、隐藏态、去重键），进程级开关只是被它们
    **轮流持有**；重新激活某会话即按**它自己**的状态重放，不会串到别的会话。
  - **隐藏分两种来源**：远端的隐藏属于会话、跨失活保留并在激活时重放；本机隐藏（Moonlight「隐藏系统光标」，
    由指针是否在画面上驱动）是**活动窗口的瞬时态**，窗口一失活就清掉，不跨会话重放。
- **去重**：键 = 内容键 + 几何 + 量化倍率，命中直接 `bufferRelease`（位图不过 NAPI）。内容键按协议取：
  RDP 服务端指针缓存命中会原样重发位图、FreeRDP 不给形状身份，故原生解码时附一个 FNV 哈希；
  Moonlight（foundation-sunshine）协议自带 `shape_id`，直接透传使用，**不做像素哈希兜底**
  （兜底成本是每次把位图搬过边界，恰好抵消优化）。命中只意味着「形状相同」，此时唯一可能的状态分歧是可见性
  （隐藏不动已安装形状与键）：**命中且处于隐藏态且事件为显示**时，用保留的 PixelMap 恢复显示并重装，
  不重建位图；否则纯 no-op。
- **超分辨率下按同一倍率放大位图**（RDP 链路）：服务器按**会话分辨率**的像素尺度发光标位图，开了超分就比画面
  小 `输出÷会话` 倍，所以原生随形状事件上报 `desktopMagnification_`（输出÷会话，会话建立时算定），
  ArkTS 按同一倍率放大再装（放大带锐化，缩小不带）。不走 GPU 超分的原因：位图最终要变成 `PixelMap`
  交给系统光标，本来就得回到 CPU，走 GPU 只是多一次提交与回读，且透明通道在 GPU 侧无法稳妥处理。
  **预乘 alpha 是必须的**：透明区 RGB 是 0，直接对直通 alpha 彩色做线性滤波会把黑混进边缘（黑边）。
  **256 系统上限**：放大后超限就退回面积平均缩小（热点随之缩放）；任何协议的源位图超过 256 同样缩小而非丢弃。

## 11. 共享单元（指针）

`model/SessionSignal`（协议无关信号）、`services/ToolbarAutoHide`（工具栏自动隐藏判定与触屏唤出/收起的提示窗口）、
`services/TouchRevealGesture`（触屏顶部下拉唤出判定器：持有触点、判定后回放或丢弃）、
`services/RemoteCursor`（远端光标安装器）、`services/ForegroundReconnect`（回到前台自动重连）、
`services/ClipboardController` + `services/ClipboardBridge` + `utils/LocalPasteboard`
（协议中立的手动剪贴板：远端缓存 / 复制粘贴语义 / 本机读写；见 [`rdp-clipboard.md`](rdp-clipboard.md) §0）、
`components/SessionChrome`（会话 chrome 常量与组件，含共享遥测行 `TelemetryRow` / `TelemetryField` / 阈值着色 `metricColor`
/ 定宽延迟文本 `formatLatency` 与共享复制/粘贴按钮 `ClipboardButtons` / 传输进度行 `ClipboardTransferStatus`）、
`utils/UiMotion`（hover/press 动效）——均为两协议共用。

## 12. 命名与边界约定

- **通用层文件 / 类不带协议名**；协议专属一律带 `Rdp` / `Moonlight` 前缀（`RdpNative`、
  `RdpSessionBackend`、`RdpLauncher`、`RdpTelemetry`、`Moonlight*`）。
- **模型分文件**：`model/Connections.ets` 放共用模型（含抽象基类与两个子类），`model/RdpModels.ets` 放 RDP 专属。
- **协议判断只允许出现在**协议专属类与唯一组合入口（`ConnectionLauncher` / `SessionBackendFactory`）：
  `if (protocol === …)` / `isMoonlight()`（已删除）/ `instanceof XxxConnection` 不得出现在通用层。
- 会话页内 `backend instanceof RdpSessionBackend` / `instanceof MoonlightSessionBackend` 取自身后端属
  协议专属页，允许。
