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
- 协议实现各自成文件：`services/RdpSessionBackend.ets`（`RdpSessionBackend` + `RdpInputSink`）与
  `services/MoonlightSessionBackend.ets`（`MoonlightSessionBackend` + `MoonlightInputSink`）。

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

**分层**：输入的**策略**（来源识别、移动合帧、按键/按钮记账、失焦全量释放、锁键顺序、Win 键替代、
穿透路由、触控板滚动与中键合成、捏合、触屏状态机）集中在协议中立的 `services/SessionInput.ets`，
由 `InputSink` 接口下发（`key/metaKey/unicode/lockState/pointerMove/button/touch/touchpadFrame/scroll`）。
两协议各一个 sink 落地动词：**`RdpInputSink`**（`RdpSessionBackend.ets`）与 **`MoonlightInputSink`**
（`MoonlightSessionBackend.ets`）——RDP 的落地细节见 [`rdp-session.md`](rdp-session.md)，Moonlight 见
[`moonlight-native.md`](moonlight-native.md)。会话页只把 ArkUI 事件转给 `SessionInput` 并注入配置
（指针映射、指针空间、各项设置与能力），**不自持输入机制**。

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
- **鼠标移动要合帧**：`onMouse` 的 Move 按设备轮询率到达（高回报率鼠标/触控板可达数百 Hz），逐条转发
  = 每条一个输入报文，且**链路越快背压越小、越拦不住这股流量**。故移动按 `MOUSE_MOVE_MIN_INTERVAL_MS`
  上限节流（约 125Hz）：只保留最新位置、必要时补一次尾帧，保证指针不会停在按下/抬起之外的位置；
  **任何非 Move 指针事件（按下/抬起）先冲掉待发移动**，否则点击会落在旧坐标上。这是**输入节流，不是显示帧率**。
- **触屏**（只认 `event.source === SourceType.TouchScreen`；手指 id `+1` 避开 0 空槽）：
  - `handleMouse` 要过滤同源兼容鼠标事件，避免"一触两通路"产生杂点击；
  - 重复 `TouchType.Down` 忽略（补发 UP+DOWN 会变成"松开+重按"的假点击）；
  - `TouchType.Cancel` **不代表抬指**（move 会被打断）：挂起不抬指，若附近随后出现新的 Down/Move
    判定为同指续接触、只发 MOTION，超过 `CANCEL_HOLD_MS` 无续接触才真正 UP；
  - 远端的长按右键菜单依赖**触点状态序列合法**：触点是按帧合并发出去的，一个触点的**首条上报必须是
    DOWN**；首条若成了 UPDATE/UP，远端不会 engage 它——长按在远端成不了 press-and-hold，还会留下一个
    不消失的触点视觉。应用侧只需保证 Down/Up 成对、**不给同一手指重复下发 DOWN**；合并后的合法性由
    协议侧兜住（RDP 见 [`rdp-session.md`](rdp-session.md)）；
  - 压力按透传（`[0,65535)` → 归一化，0 表示设备未上报）。
- **鼠标滚轮**（非触控板的轴事件）：轴值是**角度**，与触控板**同一条累加路径**——按 ArkUI 自己的换算折成
  px 后交给同一 `TouchpadWheelMapper`：1 度 → `LINE_HEIGHT_DESKTOP/MOUSE_WHEEL_DEGREES`（=21/15）vp，
  再 `× dipScale` 折成 px（ArkUI 的 `Dimension.ConvertToPx()`；本端取 `defaultDisplay().densityPixels`，
  见 `SessionInput.pxPerVp()`）。故与触控板同口径（1 px = 1 远端 px）且**保留高分辨率**（亚齿/多齿不丢）。
  **不读 `scrollStep`**：本机每齿行数已折在轴值里（MMI `rows × libinput` 每齿），故本机调快则远端同步变快。
  滚轮用独立 mapper，不受「触控板滚动速度」设置影响。
- **触控板双指滚动**（`sourceTool === TOUCHPAD`）：`axisVertical/Horizontal` 是本次事件的**位移**
  （与 ArkUI 内容位移同单位）⇒ `services/TouchpadWheel.ets` 按 `120 / VP_PER_DETENT × 速度倍率`
  累加成高分辨率轮转量。口径是**内容口径**（与窗口/画面尺寸无关，缩小窗口不改变手感，故不按画面缩放
  折算）：`VP_PER_DETENT` = 远端每齿的内容量，标称 `3 行 × 16px = 48`（Windows 默认
  `SPI_GETWHEELSCROLLLINES=3`），再按实测标定为 **192**（对鸿蒙原生约 4× 偏快）；远端为非常规行高/每齿
  行数属用户或应用有意设置，不探测、不纠正。设置「滚动速度」只缩放轮转量、不改变发射粒度。
  **原生轴值在 MMI 侧已先过一条按滑动速度分段的「加速曲线」**（`service/mouse_event_normalize` → Rust
  `get_axis_gain_touchpad`，非捕获模式直接以曲线值替换轴值）——我们读到的是**加速后**的轴值，故与原生
  一样天然非线性，**无需复刻曲线**。
- **触控板抛滑**（抬手后惯性）：原生 `Scrollable` 只对**触控板**轴手势 fling（`IsMouseWheelScroll` 排除
  `TOUCHPAD`，滚轮不 fling）。`SessionInput` 按原生模型补一段：短窗口估速 → `距离 = 速度 × 1.5 / (friction
  × |FRICTION_SCALE|)`，按**临界阻尼弹簧** `x(t) = 1 − (1+ωt)e^{−ωt}`（`ω = friction × |FRICTION_SCALE|`）
  逐帧发轮转量；低于阈值（240）不发。常量取自 ArkUI（`FLING_*`）。
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
- **锁定键（Caps / Num / Scroll Lock）路径**：读本机锁定态并与已下发值比对后同步远端；RDP 的 PDU 落地见
  [`rdp-session.md`](rdp-session.md)，Moonlight 由主机自行维护。
- **捏合与触控板的协议归属**：捏合（各三档）、触控板双指滚动、双指双击→中键都在 `SessionInput`，**两协议共享**；
  各协议的落地（RDP 的 RDPEI/直达缩放，Moonlight 的 `LiSendTouchEvent`/原生 precision touchpad）见对应协议文档。

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
  悬停上报 `TOOLBAR_ZONE_TRIGGER`、移出上报 `NONE`）与**工具栏自身**（悬停上报 `TOOLBAR_ZONE_BAR`）
  共同上报同一格（`TOOLBAR_ZONE_*`）：全屏下工具栏盖住二者，指针在工具栏上时都收不到事件。所有触发
  （指针移动 / 悬停、传输起止、全屏切换、会话状态变化）只重新求值这一次判定，不各自管定时器，
  过期定时器因此不会把工具栏从指针底下收走。
- **触发带有进度提示**：指针在顶部触发带停留期间，屏幕顶边有一条**随停留进度填充的细线**（时长与
  `hoverDelay` 一致，因此填充完成与工具栏滑出同时发生；指针一离开触发带就回退消失），预告工具栏
  即将出现。它只是提示，不参与上面的判定：指针事件必须能穿过它到达画面与工具栏。

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

`model/SessionSignal`（协议无关信号）、`services/ToolbarAutoHide`（工具栏自动隐藏判定）、
`services/RemoteCursor`（远端光标安装器）、`services/ForegroundReconnect`（回到前台自动重连）、
`services/ClipboardController` + `services/ClipboardBridge` + `utils/LocalPasteboard`
（协议中立的手动剪贴板：远端缓存 / 复制粘贴语义 / 本机读写；见 [`rdp-clipboard.md`](rdp-clipboard.md) §0）、
`components/SessionChrome`（会话 chrome 常量与组件，含共享遥测行 `TelemetryRow` / `TelemetryField` / 阈值着色 `metricColor`
与共享复制/粘贴按钮 `ClipboardButtons` / 传输进度行 `ClipboardTransferStatus`）、
`utils/UiMotion`（hover/press 动效）——均为两协议共用。

## 12. 命名与边界约定

- **通用层文件 / 类不带协议名**；协议专属一律带 `Rdp` / `Moonlight` 前缀（`RdpNative`、
  `RdpSessionBackend`、`RdpLauncher`、`RdpTelemetry`、`Moonlight*`）。
- **模型分文件**：`model/Connections.ets` 放共用模型（含抽象基类与两个子类），`model/RdpModels.ets` 放 RDP 专属。
- **协议判断只允许出现在**协议专属类与唯一组合入口（`ConnectionLauncher` / `SessionBackendFactory`）：
  `if (protocol === …)` / `isMoonlight()`（已删除）/ `instanceof XxxConnection` 不得出现在通用层。
- 会话页内 `backend instanceof RdpSessionBackend` / `instanceof MoonlightSessionBackend` 取自身后端属
  协议专属页，允许。
