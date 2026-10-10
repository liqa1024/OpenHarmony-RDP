# 会话窗口 / 输入 / 工具栏与遥测

本文件覆盖会话的**窗口生命周期与状态机、窗口尺寸 / 全屏 / 可见性、输入分流（鼠标 / 键盘 / 触屏 / 触控板）、
远端光标同步、会话工具栏与状态遥测**。剪贴板与文件传输见 [`clipboard.md`](clipboard.md)；
窗口的运行时能力（全屏检测 / 最小化切换 / 焦点）归 `services/WindowController`，其配置项见
[`settings-and-storage.md`](settings-and-storage.md)。

## 1. 会话窗口模型

- 每个会话在**独立的 `SessionAbility`** 主窗口中打开（`launchType: specified` + 唯一 `instanceKey`）。
  必须用独立 Ability 才能拿到完整标题栏（子窗没有最小化按钮）。
- 每次连接生成 `sessionKey`；待连接参数放 `SessionRequests`，**Want 里只带 key（密码不进 Want）**。
- **主窗口后台连接、成功后才开窗**；失败只报错、不开窗。每个会话独占一个**会话后端**
  （`SessionBackend`；RDP 实现 `RdpSessionBackend` 内部独占一个 `RdpNative` 实例）。
- **关闭窗口即断连**：`SessionAbility.onWindowStageDestroy/onDestroy` → `SessionManager.release` +
  `SessionRequests.discard`。注意 `SessionPage.aboutToDisappear` 在窗口关闭时**不触发**，别依赖它断连。
- 连接有**超时兜底**；错误/断开原因经 `SessionManager.describeError` 分类（原生格式 `<错误码>|<消息>`）。
  - 码的高 16 位是 FreeRDP 错误类：`1` = **ERRINFO**（服务端 ErrorInfo/断开原因），`2` = **ERRCONNECT**
    （本地连接失败）。原生在断开时优先报服务端 ErrorInfo（经 `freerdp_error_info` 取回，如
    `ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION` = 被另一个连接接管），否则报客户端 last error；
    **用户主动断开**没有原因，payload 为空。
  - 断开弹窗据此区分"被其他连接接管 / 空闲超时 / 远端注销 / 服务端驱动或系统进程异常 / 网络中断"，
    而不是笼统的"连接已断开"；空 payload 才回落到 `连接已断开`。
- **连接/断开由一个状态机统一管理**：`SessionManager` 是唯一权威，状态为
  `Idle → Connecting → Connected → Disconnecting → Idle`，失败走 `Failed`。会话窗口只是**视图**——
  它不创建也不销毁原生会话，只订阅该状态（渲染浮层）与原生 UI 事件（首帧/分辨率/遥测/剪贴板/光标）；
  原生监听器由 manager 持有。列表与窗口因此不可能各持一份状态。
  - **manager 不绑定具体协议**：它按 `PendingConnection.protocol` 向 `SessionBackendFactory` 取一个
    `SessionBackend`（`services/SessionBackend.ets`），只消费协议无关的 `SessionSignal`（生命周期
    `Connected/Disconnected/Error` + 协议自带码的 `UiEvent`，见 `model/SessionSignal.ets`）与协议无关的
    输入动词（指针/触摸/按键/Unicode）。协议特定的会话窗口把 `UiEvent.eventCode` 当自己的事件码解释，
    并向下转型到自己的后端取更多能力（RDP 用 `RdpSessionBackend.native`）。`SessionBackendRegistry`
    按 `sessionKey` 关联窗口与其后端。
  - **连接过程两端同形：先发布 `Connecting`，再做异步预检**。`connect`/`retry` 一律**先发布 `Connecting`**
    再交给后端——列表行因此立刻进入加载态，与协议无关。后端可声明 **`requiresPrepare()`** 并在
    `prepare(): Promise<boolean>` 里做异步预检（Moonlight：`serverinfo → pair → launch`，配对 PIN 弹窗在此
    弹出）；预检期间 `Connecting` 已成立，所以不会出现"先无反馈、窗口里才闪一下加载"的断档。预检失败由
    后端发 `Error` 信号（文案即连接失败原因），窗口不开；`prepare()` 返回 `false` 而未发信号时由 manager 兜底
    （发布 `Failed`、停后端）。预检**不挂连接超时**（配对要等用户输入），超时只在真正开始启动后才计。
  - 后端另可声明 **`requiresWindowAtStart()`**（Moonlight：`LiStartConnection` 需要先有渲染目标）——预检
    通过后 manager 才开窗，窗口绑好 XComponent surface 后调 `SessionManager.beginStream` 真正启动；RDP
    为 false，维持"连上后才开窗"。Moonlight 会话窗口与后端 `MoonlightSessionBackend` 同此模型。
  - 原生 `Session` 的生命周期也是**单向**的（`kIdle→kStarting→kConnected→kStopping→kDestroyed`，
    一次尝试一个对象、不复用）：只有 RDP 线程释放 FreeRDP context（`Teardown`），其它线程（输入/剪贴板/
    gdi 回调）须先 `AcquireContext()` 取租约、用完 `ReleaseContext()`，`Teardown` 等**在途租约归零**后
    才释放，并由 `WaitDestroyed` 发布"已彻底回收"。**新代码碰 context 一律走这条路径，不要再裸读
    `instance_`/`cliprdr_`**。
- **一次尝试 = 一个全新原生会话 + 一个 epoch**：`connect`/`retry` 都作废上一个 epoch，事件的 epoch
  不是当前值即被丢弃，所以被替换或已关闭会话的迟到事件不会驱动当前会话。
- **终结有界且可等待**：`stopSession()` 向 RDP 线程发停止信号后**等待会话真正释放**（有界；
  若等待租约归零超时，则**放弃（泄漏）该 context 而不是释放它**——宁可泄漏也不在活着的使用者脚下
  释放，并记警告；这条路径正常不会走到），释放完成后才允许新尝试。
  故 `Connecting`/`Connected`/`Disconnecting` 一律拒绝新尝试（不拆旧会话）；一次尝试 = 一次完整协商 +
  远端一次登录，另一端是**断开**（远端会话保留为已断开 `Disc`，不注销），所以连点"重试"或双击列表
  **不会变成连接/登录风暴**。`MIN_ATTEMPT_INTERVAL_MS` 只是次级保护（被拒绝的重试不返回成功，调用方
  不得据此切"正在连接"状态）。节流按 `connectionId` 记账、**在 entry 被清理后仍然保留**。
- **失败保留 entry 并只有一个终态事件**：连接失败（含超时）置 `Failed` 且不删 entry，窗口「重试」与
  列表再次点击据此发起新尝试；失败只产生 `kError`（不另发 `kDisconnected`），失败原因不会被
  "连接已断开"覆盖。
- **回到前台自动重连**（全局设置 `reconnectOnForeground`，默认关）：会话窗口从后台回来时若发现会话已断，
  自动重连一次，省去手点「重试」。它**不是保活**，只兜"回来时已经断了"这一次：应用退到后台后，
  系统会先冻结网络资源（约 2s）、随后释放并 abort socket（约 12s），而官方对终端类会话
  （SSH/Mosh/SFTP）的判定是**不属于合法的高优先级后台服务**，普通三方应用无法用长时任务撑住这种
  "网络空载 / 低频心跳"（见 [`settings-and-storage.md`](settings-and-storage.md) §2）。
  - 触发条件是**真的离开过**（`onPageHide` 或 `onApplicationBackground` 置位），回来后**延迟一个宽限期**
    才判定：原生读线程要恢复运行之后才会把断开事件报上来，立刻判定会读到还没更新的状态。
  - 只接管 `Idle`（已断） / `Failed`。用户主动断开/返回不在此列；前台正常使用中掉线也仍走手动「重试」，
    不会被这里变成自动重连循环。
  - PC/2in1 上**窗口最小化不会驱动 UIAbility 进入后台**，因此这条路径在 PC 上通常根本不触发。

### 1.1 窗口尺寸与全屏

- 默认尺寸按屏幕比例推导，屏幕查询失败则**不改窗口**；也可在设置里手动指定，或（会话）默认最大化。
  **尺寸只在窗口创建时生效**；`resize`/`moveWindowTo` 的单位是 **px**（不是 vp）。
- `sessionFullscreen` 经 `StartOptions.windowMode = WINDOW_MODE_FULLSCREEN` 直接全屏；
  会话窗的**系统最大化**在 `WindowController.setupSessionWindow` 里转成**沉浸式全屏**
  （隐藏标题栏 / dock 悬停）。
- **窗口不可见期间不应用 surface 尺寸变化**：最小化/完全遮挡时系统会重新分配它的 surface，恢复时先报过渡
  尺寸。故不可见期间只挂起，重新可见后再取一次 surface rect 应用；尺寸没变不下发。
- **不可见时主动抑制服务端输出**（MS-RDPBCGR Client Suppress Output，`gdi_send_suppress_output`）：不可见即发
  suppress，服务端停发画面更新、客户端丢弃 surface 命令，停止解码/合成/上屏；可见后解除，画面由服务端重发。
  可见性口径只有两条，来自两种回调、可能只来其一：`windowVisibilityChange`（完全遮挡）与 `windowStatusChange`
  的最小化，二者任一为"不可见"即算不可见。**部分遮挡返回 `true`**，"全屏会话被最大化窗口压住"判不到（系统不
  提供遮挡比例与被谁遮挡）。焦点、`windowEvent` 的 hidden/shown、页面生命周期都不是可见性判据（混写同一个状态
  只会互相覆盖而漏抑制），生命周期只承担"回来时是否已断"的重连记账。服务端不支持该能力时只省本地工作、不省带宽。

### 1.2 自动隐藏主窗口（单窗口模式）

全局设置 `autoHideMainWindow`（默认关）：开启后仍**新建** `SessionAbility`，但**销毁主 `EntryAbility`**
以真正隐藏（没有 hide API；`minimize()` 仍留在 Dock）。按**单会话**设计，故 `WindowController` 只用
`mainHidden` 布尔量、不跟踪会话集合。

- **进程内最后一个 UIAbility 被销毁 ⇒ 进程退出**：必须等会话窗加载完成后再 `terminateSelf()` 主窗口，
  否则会把整个应用杀掉。
- 关闭会话时**先拉起主窗口**，主窗口就绪后再终止会话；用 `windowStage.on('windowStageClose')` +
  `UIAbility.onPrepareToTerminate()` 拦截关闭（后者在部分设备上不触发），`closeSession` 去重 +
  超时兜底。
- 关闭该选项则行为不变（多窗口可并存）。

## 2. 输入分流

**分层**：输入的**策略**（来源识别、移动合帧、按键/按钮记账、失焦全量释放、锁键顺序、Win 键替代、
穿透路由、触控板滚动与中键合成、捏合、触屏状态机）集中在协议中立的 `services/SessionInput.ets`，
由 `InputSink` 接口下发（`key/metaKey/unicode/lockState/pointerMove/button/touch/touchpadFrame/scroll`）。
两协议各一个 sink 落地动词：**`RdpInputSink`**（`SessionBackend.ets`：PS/2 scancode、`PTR_FLAGS`、RDPEI 触点、
9bit 滚轮分片、Client Synchronize）与 **`MoonlightInputSink`**（`MoonlightSessionBackend.ets`：Windows VK、
`LiSend*`）。会话页只把 ArkUI 事件转给 `SessionInput` 并注入配置（指针映射、指针空间、各项设置与能力），
**不自持输入机制**；**RDP 专有的"直接缩放"（本机画面矩阵）留在会话页**，由 `SessionInput` 之外的
`handleAxis` 分支处理，其余轴事件才委托给共用件。

**键表单表**：`utils/KeyMapper.ets` 用一张表同时给出 RDP 的 `scancode`+`extended` 与 Moonlight 的 `vk`+`extended`
（及 lock 位），避免两协议键表漂移；`mapFull` 供 sink 取用，`map` 只回 `scancode`（RDP 侧兼容）。
**左右修饰键区分**：协议与主机都区分左右（`VK_LSHIFT/RSHIFT` 等）；表发左右专用 VK，修饰位按
**当前按下的修饰键集合**求值（不是按单个 VK 翻转），所以"按住左右 Shift 松一个"不会清掉 Shift 位。

**鼠标 → `onMouse`；真触屏 → `onTouch`（RDPEI）；滚轮/触控板 → `onAxisEvent`。**

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
  Unicode 码点 / Win 替代键 `0x5B`，鼠标=`BUTTON` 标志），失焦时统一补发抬起并松开触屏与捏合；
  **新增输入通路必须进这份账**。
- **鼠标按键**：移动（`PTR_FLAGS_MOVE`）与按键事件**分开送**，按键事件**不带 MOVE 标志**，
  否则远端会忽略点击。
- **鼠标移动要合帧**：`onMouse` 的 Move 按设备轮询率到达（高回报率鼠标/触控板可达数百 Hz），逐条转发
  = 每条一个输入 PDU，且**链路越快背压越小、越拦不住这股流量**。故移动按 `MOUSE_MOVE_MIN_INTERVAL_MS`
  上限节流（约 125Hz）：只保留最新位置、必要时补一次尾帧，保证指针不会停在按下/抬起之外的位置；
  **任何非 Move 指针事件（按下/抬起）先冲掉待发移动**，否则点击会落在旧坐标上。这是**输入节流，不是显示帧率**。
- **触屏**（只认 `event.source === SourceType.TouchScreen`；手指 id `+1` 避开 FreeRDP 的 0 空槽）：
  - `handleMouse` 要过滤同源兼容鼠标事件，避免"一触两通路"产生杂点击；
  - 重复 `TouchType.Down` 忽略（补发 UP+DOWN 会变成"松开+重按"的假点击）；
  - `TouchType.Cancel` **不代表抬指**（move 会被打断）：挂起不抬指，若附近随后出现新的 Down/Move
    判定为同指续接触、只发 MOTION，超过 `CANCEL_HOLD_MS` 无续接触才真正 UP；
  - 远端的长按右键菜单依赖**触点状态序列合法**：触点是按帧合并发出去的，一个触点的**首条上报必须是
    DOWN**；首条若成了 UPDATE/UP，远端不会 engage 它——长按在远端成不了 press-and-hold（不出右键菜单），
    还会留下一个不消失的触点视觉。应用侧只需保证 Down/Up 成对、**不给同一手指重复下发 DOWN**；合并后的
    合法性由 RDPEI 侧兜住（见 [`native-libraries.md`](native-libraries.md) §3）；
  - 压力按 `HAS_PRESSURE` 透传（`[0,65535)` → `[0,1024]`，0 表示设备未上报）。
- **触屏高刷新率**（全局设置）：FreeRDP `rdpei` 默认约 50Hz 合帧，是与桌面客户端的主要差距；
  补丁导出的运行时全局（弱符号引用）在开启时把合帧间隔**从 20ms 降到 8ms**（50Hz → 125Hz）。
  这个间隔是**刷新周期的下限**，也就是远端触屏输入速率的**上限**：取 0 等于"每次输入轮询都发"，
  速率就只由网络决定 ⇒ 一次拖拽变成无界的接触点流。未打补丁的 FreeRDP 上自动降级。
- **触控板双指滚动**（`sourceTool === TOUCHPAD`）：`axisVertical/Horizontal` 是本次事件的 **vp 位移**
  而非轮齿 ⇒ `services/TouchpadWheel.ets` 按 `120/16vp × 速度倍率` 累加成**高分辨率 RDP 轮转量**
  （9bit 二补码、120=1 齿、单事件 ≤0xFF 分片，对齐 FreeRDP 的 SDL 实现），横向发 `HWHEEL`。
  设置「滚动速度」只缩放轮转量、不改变发射粒度。
- **触控板捏合**（设置「触控板两指捏合模式」，三选一）：
  - **Ctrl+滚轮**（默认）：按 `1/(20×速度)` 线性映射为 **Ctrl+滚轮**（「缩放速度」），由远端应用自己缩放。
  - **触摸模拟**：在鼠标位置**合成两个原生触点**做真实双指缩放——距离 1:1、初始间距取窗口较短边的百分比
    （默认 8%，不写死分辨率）、按设置角度斜置。**不设计时器**，只在真实 `AxisAction.END` 结束。
    - 起手 **30ms 内不发 MOTION**：避免 RDPEI 50Hz 合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时捏合会失效）。
    - ArkUI `AxisType` **没有旋转轴**，拿不到真实手指朝向（多指不上报手指信息），所以用角度设置顶替。
    - **捏合期间给一个定位光标**：合成了真实触点后，系统会把鼠标指针隐藏（触屏活动态），此时若开着
      「RDP 光标」就完全看不到捏合锚点。于是捏合期间（`RemoteCursor` 的捏合覆盖）把系统光标换成鸿蒙原生
      **手势手型**并强制可见：**张开手**（`HAND_OPEN`）= 手指张开/放大，**抓取手**（`HAND_GRABBING`）
      = 手指收拢/缩小；方向按手势比例判、带 2% 死区避免抖动来回切，只在方向真的翻转时才改一次样式
      （`setPointerStyleSync` 是系统调用，不能每帧调）。结束时恢复远端位图/隐藏态。
  - **直接缩放**：**不向远端发任何数据**，只在本机把画面放大，所以会话分辨率、码流与帧率都不变。
    以**光标为锚点**（其下的远端像素保持不动），**1.0 是拟合（letterbox）视图** ⇒ 下限 100%、上限
    8×，手势比例直接当倍数（不叠加「缩放速度」）。
    - 变换是给 XComponent **外层容器**的一个 ArkUI 矩阵（`matrix4`，以组件中心缩放 + 屏幕平移），
      纯合成层缩放、不碰像素：状态一变即上屏，静止画面也能看到，不依赖远端再出一帧。
    - **指针映射不变**：指针事件按组件的**未变换局部坐标**上报，`mapPointer` 拿到的仍是拟合视图里的点；
      锚点、基准矩形与它同取 `baseViewRect()`，三者不会漂移。
    - **不是 100% 即缩放模式**：此时所有滚动改成画面平移（触控板位移 1:1 跟随手指，鼠标滚轮每齿一个
      固定步长）且**不发远端**；回到 100% 恢复原滚动。平移与缩放共用同一套「盖满 / 居中」夹取，不会露黑边。
    - **收手吸附**：捏合落在 100%～108% 收手时用短缓出动画回落到正好 100%，免得一次很小的捏合把会话
      停在「差一点」的缩放态（那样滚动被本机吃掉）；更大则停在松手处。
    - 远端分辨率或窗口尺寸变化会重置回 100%（拟合矩形变了，旧变换会失配）。
- **触控板双指双击＝中键**（全局设置，默认关）：触控板对应用只上报单指点触＝左键、双指点触＝右键、
  双指滑动/捏合＝轴事件，**多指与指关节都不上报**，所以中键只能自己合成。开启后把触控板的双指轻触按
  **右键**延迟一个双击窗口发出：窗口内出现第二次双指轻触就改发**中键**（单次仍发右键），因此右键有约
  300ms 判定延迟；关闭则无延迟直发。合成点击一律按**一次按下＋抬起**整发（不会卡键）。
  只对触控板来源生效（判定见上「输入来源识别」），真鼠标右键不受影响。
- **自定义 Win 键映射**（全局设置，0＝关闭）：在 `SessionInput.handleKey` 里把指定 `keyCode` 改发 Meta。
  **真实 Win 键不转发**，避免"远端+本机"双重映射。
- **快捷键穿透**（全局设置 `shortcutPassthrough`，默认关）：系统级按键拦截
  （`OH_Input_AddKeyEventInterceptor`，需 ACL `ohos.permission.INTERCEPT_INPUT_EVENT`）接管键盘，
  让 Win / Alt+Tab / Esc 这些本会被 shell 吃掉的键也到远端。拦截事件按**本会话**路由到
  `SessionInput.handleInterceptedKey`（映射与记账同 `handleKey`，但**不跳过真实 Win**）；
  拦截期间 `handleKey` 不参与，避免双发。**归属按获焦窗口**：进程级单拦截器，只有获焦会话持有，
  切窗/失活即释放（见 §1 的窗口焦点与 `WindowController`）。（与"自定义 Win 键映射"不同，穿透下
  **真实 Win 键会被转发**。）
  - 拦截是**进程级、且只在应用获焦时投递**，故只在**本会话窗口活动**且会话 `Connected` 时注册；其他窗口
    （设置页 / 主列表）变为活动、或本页离开即释放，否则那些窗口的键盘也会被吞掉。
  - 代价是开启期间**本机的系统快捷键一并被远端接管**，所以默认关；注册失败（缺 ACL 等）时原生返回
    `false`，UI 保持普通按键通路而不是把键盘丢掉。
- **锁定键（Caps / Num / Scroll Lock）以本机为准，RDP 跟随本机**：任何按键事件都读本机锁定态
  （`KeyEvent.isCapsLockOn` / `isNumLockOn` / `isScrollLockOn`，API 19；设备不给这几个字段时保持远端现状）
  并与已下发值比对，变了就发 RDP `Client Synchronize PDU`（`freerdp_input_send_synchronize_event`，
  位值与 `KBD_SYNC_*` 一致）。**非锁定键在转发前发**——这样该键的字符大小写已经正确，覆盖"焦点不在本窗口时
  本机锁定态被改"的情况；**锁定键本身在转发后发**——该键自己已经让远端翻转，先发会变成二次翻转。服务端的
  `Set Keyboard Indicators` PDU（`update->SetKeyboardIndicators`）报远端锁定态，其客户端默认实现是
  **把 PDU 原样回发**，必须接管。**原生只把这个报告转成 `RemoteLockState` 事件**（不做比对）；
  **与本地是否一致、要不要纠正由 ArkTS 决定**（`SessionPage.onRemoteLockState`：不一致且距上次重发 ≥1s 时调
  `reassertLockState` 发一次**无去重**的 Client Synchronize，避免与服务端来回拉锯）。本机态由按键事件里的
  `isCapsLockOn`/`isNumLockOn`/`isScrollLockOn` 记录（`localLockFlags`）。三个锁定键（Caps 0x3A、Scroll 0x46、
  Num 0x45）与各自的小键盘扫描码都要在 `KeyMapper` 里齐全，否则本机锁定键根本到不了远端。
- **远端光标同步**（全局设置，默认开）：RDP 只传光标**位图**（系统指针仅 `SYSPTR_NULL`/`SPTR_DEFAULT`），
  所以照搬位图、**不做类型映射**；HarmonyOS 会把自定义位图缩放到固定的系统光标大小，故**无需**按
  画面/位图尺寸自行缩放（位图大小只影响观感锐度）。RDP 与 Moonlight 共用同一安装器
  `services/RemoteCursor.ets`：原生只发**解码后**的 BGRA + 热点 + 内容键 + 桌面倍率，策略全在 ArkTS。
  （**模拟器无鼠标，只能真机验证**。）
  - **分层**：必须原生的只有两样——协议解码（RDP 的 xor/and 掩码 `freerdp_image_copy_from_pointer_data`；
    Moonlight 库直接给 BGRA）与重采样数值核（`core/hmrdp_bitmap`：预乘 alpha 面积平均缩小 / Lanczos3 +
    对比自适应锐化放大，经 NAPI `scaleBgraBitmap` **句柄进句柄出**，位图不为变换本身穿越边界）。其余全是
    ArkTS 策略：目标尺寸（源 × 倍率，按比例钳 ≤256 系统上限）、热点换算、PixelMap 构建与安装、捏合定位光标、
    无鼠标设备的能力探测、可见性与默认形状状态机。安装器**全权消费句柄**：任何分支都会
    `bufferRead`/`bufferRelease`/转交缩放，句柄不可能泄漏。
  - **状态机不变式**：系统光标始终等于「最近一次送达的（形状，可见性）」。形状事件**蕴含显示**——
    RDP 没有独立的「显示」事件（只有 `SetNull` 隐藏），形状或默认箭头到达即是显示；Moonlight 的
    `visible` 标志可随形状同发，以其为准（改形状但保持隐藏是合法组合）。`Default` 清形状回默认箭头。
    - **重放点**：正确性**不依赖协议重发**（RDP 每次指针更新都重发形状，Moonlight 只在 `shape_id`
      变化时发一次），而由安装器在「就绪 / 可能失效」的时机主动重放当前状态。失效时机有两个且
      **互不包含、时序不定**：窗口激活（`setWindow` 里订阅 `windowEvent` 重放；激活重置可以落在
      hover 重放之后，使后者失效——两个会话窗口互切时 RDP 靠协议重发自愈、Moonlight 只能靠激活
      重放）与指针重进窗口（两个会话页的 `onHover` 都调 `reapply`）。窗口绑定本身也是重放点
      （`setWindow` 里即重放——窗口是异步获取的，首个形状事件可能先到，状态先装着）。任何新增的
      消费方或失效场景都应挂到重放上，而不是指望对端再发一次。
    - **进程级可见性只有一个、按活动窗口唯一归属**：形状**按窗口**安装
      （`setCustomCursorSync`/`setPointerStyleSync` 带 `windowId`），但显示/隐藏是**进程级**的
      （`setPointerVisibleSync` 对进程内所有窗口生效）。所以可见性不能由每个会话各自设置：安装器把它
      **按「活动窗口」唯一归属**（静态 owner），非活动窗口只保留自己的状态、不碰系统开关；窗口失活 /
      隐藏 / 销毁时**交还**开关并恢复显示——若开关已被新活动窗口抢先认领则不动（避免覆盖它刚设的隐藏）。
      没有这层归属，一个把光标隐藏的会话切走后会把主窗口的光标也留在隐藏态；会话窗口关闭**不跑页面
      teardown**，`windowEvent` 这条失活路径才是可靠的收尾点。
    - **会话间状态独立**：每个会话页持有自己的安装器实例（形状、隐藏态、去重键），进程级开关只是被
      它们**轮流持有**；重新激活某会话即按**它自己**的状态重放，不会串到别的会话。
    - **隐藏分两种来源**：远端的隐藏（RDP `SetNull` / Moonlight 的 `visible` 标志）属于会话、跨失活
      保留并在激活时重放；本机隐藏（Moonlight「隐藏系统光标」，由指针是否在画面上驱动）是**活动窗口的
      瞬时态**，窗口一失活就清掉，不跨会话重放。
  - **去重**：键 = 内容键 + 几何 + 量化倍率，命中直接 `bufferRelease`（位图不过 NAPI）。内容键按
    协议取：RDP 服务端指针缓存命中会原样重发位图、FreeRDP 不给形状身份，故原生解码时附一个 FNV
    哈希；Moonlight（foundation-sunshine）协议自带 `shape_id`（"id 变 ⇒ 形状变"，且只在 id 变化时
    发形状），直接透传使用，**不做像素哈希兜底**（兜底成本是每次把位图搬过边界，恰好抵消优化）。
    命中只意味着「形状相同」，此时唯一可能的状态分歧是可见性（隐藏不动已安装形状与键）：**命中且
    处于隐藏态且事件为显示**时，用保留的 PixelMap 恢复显示并重装，不重建位图；否则纯 no-op。
  - **超分辨率下按同一倍率放大位图**：服务器按**会话分辨率**的像素尺度发光标位图，开了超分就比画面
    小 `输出÷会话` 倍，所以原生随形状事件上报 `desktopMagnification_`（输出÷会话，会话建立时算定），
    ArkTS 按同一倍率放大再装（放大带锐化，缩小不带）。不走 GPU 超分的原因：位图最终要变成
    `PixelMap` 交给系统光标，本来就得回到 CPU，走 GPU 只是多一次提交与回读，且透明通道在 GPU 侧
    无法稳妥处理。**预乘 alpha 是必须的**：透明区 RGB 是 0，直接对直通 alpha 彩色做线性滤波会把
    黑混进边缘（黑边）。**256 系统上限**：放大后超限就退回面积平均缩小（热点随之缩放）；任何协议
    的源位图超过 256 同样缩小而非丢弃。

- **捏合与触控板的协议归属**：上面三档捏合（Ctrl+滚轮 / 触摸模拟 / 直接缩放）与触控板双指滚动、
  双指双击→中键都在 `SessionInput`，**两协议共享**；只有"直接缩放"改本机画面矩阵、属 RDP 呈现器专有，
  Moonlight 不暴露。**"原生触控板"是另一个终点**：`InputSink.touchpadFrame(contacts, buttonState, wMm, hMm)`
  表达它，仅 foundation-sunshine 主机有（`SS_TOUCHPAD` 扩展）；基础协议（含 Moonlight、RDP）都没有，
  故只能落到 Ctrl+滚轮或触点模拟。客户端接入方式见 [`MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)。

## 3. 工具栏与状态遥测

- **显示规则**：全屏模式沿用**悬浮自动隐藏**（鼠标靠近屏幕顶部才滑出；触发/隐藏延迟在
  **开发者选项**里调）；窗口模式**始终显示**，且作为普通行布局在远程画面**上方**（渲染区自然扣除工具栏高度、
  不被覆盖，指针映射仍以 XComponent 局部坐标为准）。右侧按钮：复制 / 粘贴 / 全屏·退出全屏 / 最小化 / 断开。
  开发者选项开启时，遥测（网络 / 本机 / FPS / 带宽 / 音频）另起**第二行**，主行布局与非开发者模式
  **完全一致**（工具栏变高，`toolbarHeight` 随之增长，全屏自动隐藏的判定边界跟着用这个值）。
- **自动隐藏的判定口径**：全屏下只看**指针在哪一格**——顶部触发带（贴顶一条窄带，停留 `hoverDelay` 才
  滑出）、工具栏自身、其余画面；**指针在工具栏上 ⇒ 保持显示，且不允许任何隐藏定时器在跑**，在画面里 ⇒
  起 `hideDelay` 收起。指针格由**画面**（鼠标事件）与**工具栏自身**（悬停事件）共同上报同一格
  （`TOOLBAR_ZONE_*`）：全屏下工具栏盖住画面，指针在工具栏上时画面收不到任何事件。所有触发
  （指针移动 / 悬停、传输起止、全屏切换、会话状态变化）只重新求值这一次判定，不各自管定时器，
  过期定时器因此不会把工具栏从指针底下收走。
- **触发带有进度提示**：指针在顶部触发带停留期间，屏幕顶边有一条**随停留进度填充的细线**（时长与
  `hoverDelay` 一致，因此填充完成与工具栏滑出同时发生；指针一离开触发带就回退消失），预告工具栏
  即将出现。它只是提示，不参与上面的判定：指针事件必须能穿过它到达画面与工具栏。
- **主行的分辨率是会话（超分辨率前）分辨率**，不是设置里的输出分辨率；开了超分辨率时在后面补一个
  括号，**先是后端名再是倍率**（` (FSR 1.5x)` / ` (XEngine 1.5x)`，`SessionPage.srIndicator()`，两者都取
  连接选项，见 [`settings-and-storage.md`](settings-and-storage.md) §2）。会话首帧到达前该值按
  `SettingsStore.srSessionSize()` 预置，收到 `kResize` 后以服务端实际桌面为准。

- **帧工时计量（`core/hmrdp_gfx_work.{h,cpp}`，live 与回放共用）**：同一个 `GfxWorkMeter` 由**同一批钩子**
  喂数（抓取钩子取 chunk 到达时刻，在 ZGX 之前、**落盘之后**；链式包裹的 `SurfaceCommand` 计解码；
  包裹的 `EndFrame` 计合成 + 上屏），所以**同一份码流在 live 与回放里各相位定义完全相同**，可逐相、
  按载荷对照。计量器**只产子项、不存总数**：各相位语义不同（CPU 工作 / GPU 提交 / 阻塞等待），合适的和
  由展示端相加，`sync` 这类阻塞项因此天然不计入 live 的合计。

- **工具栏左侧遥测**每秒刷新一次：原生在 RDP 线程按 ~1s 采一个**原始样本**（`sessionTelemetry()`——
  该窗口的收发/帧增量与工作/音频/GPU 子项，源在采样时清零；采样留在 RDP 线程，故不会与管线拆除竞争），
  **聚合在 ArkTS**（`services/SessionTelemetry`）：它把样本按自身窗口时长/帧数换算成速率与每帧均值，
  原生不产任何展示用速率。字段：
  - **本机**（µs/帧）= 四个**工作**相位之和（`zgx+parse` / `decode` / `compose` / `present`）；**`sync` /
    `presentWait` 是阻塞子项**（分别等 GPU 放开主缓冲、等显示端），单列、都不入 `本机`。相位语义、
    拆相与墙钟公式（`本机 + sync + presentWait`）见 [`gfx-engine.md`](gfx-engine.md) §7.1。
  - **节拍睡眠（`pace`）是唯一直接剔除的一项**：回放的人为节流不是客户端工作。
  - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值（只有服务端注册该回调），
    故连接时给 `context->autodetect` 自行注册回调捕获。
  - **带宽**：`freerdp_get_stats()` 的收发字节差分。**FPS**：只显示**应用呈现数**（对端帧率）；
    面板刷新率（恒为 60/120 一类）不进工具栏。
  - **音频**：采样率 + 近期**丢帧率**（欠载补静音 + 队列超上界时丢掉的整包；在"数据仍在到达"时统计，
    取滑动窗口——停顿不计，流真正结束后才停；缓冲与丢包策略见 [`native-libraries.md`](native-libraries.md) §5）。
  - **CPU**：`hidebug.getCpuUsage()` 的**进程**占用率（%，每次遥测采一次）。官方把这个模块定位为
    调试接口（相对耗性能），所以只在这条 dev 独占的遥测行里、按秒采一次；设备缺该能力时回调失败，
    这一项显示 `CPU --`。系统占用率（`getSystemCpuUsage`）**不显示**：这里要回答的是"本机应用花了多少"。
  - **GPU**：**本机自己的 GPU 上屏工时/帧**（ms），由呈现器的时间戳查询分成**上传 / 超分辨率 / letterbox**
    三段（悬停提示给拆解）。它**不是 GPU 整机占用率**——平台没有对三方应用开放该接口，所以这里给的是
    "这一帧本机让 GPU 干了多久"，用来判断超分辨率本身的开销。**只有 Vulkan 呈现器能测**（GLES 兜底与无
    时间戳支持的设备回 0，显示 `GPU --`）。
  - **不显示**：服务端处理（协议不回报）、压缩比（只对 GDI 位图路径有意义）、音频丢包（复用在同一传输里，
    客户端无逐包统计）、**响应延时**（协议无回显、只能"空闲后输入→首帧"推断，噪声大，故不测量）。

- **口径约束（读数前必看）**：
  - **分母是 EndFrame 帧数**，不是呈现帧数：一次处理的工作在**它的帧收尾时**才入账，所以"一帧多大"
    （由到达节拍决定）不会跑偏分母；窗口边界最多切到一帧。
  - **拆相随内容变**：`decode` 随该帧载荷/命令数变（低 fps 时每帧扛的是累积变化，比高 fps 时大），
    `compose`/`present` 随桌面尺寸变 ⇒ 单看合计会把它误当成"客户端常量能力"。
    `bytesPerFrame`、`cmdsPerFrame` 与 **dutyPermille**（四相之和 / 墙钟）才是跨帧率可比的量：
    duty 远小于 100% 就是客户端在等数据。
  - **相位是墙钟，所以低频/被调度会放大它**：RTT 大 ⇒ 帧稀 ⇒ CPU 闲 ⇒ SoC 掉档、cache/线程池变冷 ⇒
    同一份解码的墙钟变大。**网络等待本身不在任何相位里**（到达戳在完整 chunk 之后、ZGX 解压之前），
    所以 live 相位偏大只可能来自频率/调度，不是"把 RTT 算进去了"；跨频率比相位无意义，回放侧用
    `cpuKHz=` 记档（[`gfx-engine.md`](gfx-engine.md) §7.1）。
  - **仍不含**：传输层读/drdynvc 重组（在抓取钩子之前）、**RDPGFX 帧回执**（FreeRDP 在 EndFrame
    回调返回**之后**才写）；`present` 里的 `WaitForFences`/`acquireNextImageKHR` 在合成器不还 buffer
    时会阻塞，排队/显示延迟会算进 `present`（与 `sync` 不同）。
  - 同一行每秒还有 hilog（由 ArkTS 聚合侧按同一 domain/tag 打出，字段与口径不变）：
    `perf: work X us/frame = zgx+parse … + decode … + compose … + present …
    (+ sync … blocked) (frames=… presents=… cmds/frame=… kB/frame=… duty=…%  gpu copy=… sr=… blit=…
    us/frame)`，定位用这一行。

- **显示口径**：指标用**固定宽度**排布（避免数字位数变化时重排），间距分"网络↔本机"与其余两档。
  延迟类（网络/本机）统一用**毫秒、取整到个位**；本机的**拆相放在悬停提示**里（同样毫秒整数：解压+解析 /
  解码 / 合成 / 上屏，后跟 `KB/帧` 与"本机工时占帧周期的百分比"）。`本机` 的告警阈值按"还能不能到 60/30fps"
  理解（16ms / 33ms）。GPU 一项给**一位小数**（本来就只有几毫秒，取整会看不出差别），拆解同样在悬停提示里。
  **字段顺序：固定字段在前（网络 / 本机 / FPS / CPU / GPU），会消失的字段按"出现频率从高到低"排在后面
  （带宽 - 有流量就显示 → 音频 - 播放时才显示）**：条件项越靠后，它出现/消失时能影响到的字段就越少
  （带宽的文字还随速率变宽，本身不定宽）。
