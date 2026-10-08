# 会话窗口 / 输入 / 工具栏与遥测

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
    后端发 `Error` 信号（文案即连接失败原因），窗口不开；预检**不挂连接超时**（配对要等用户输入），超时只在
    真正开始启动后才计。
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
  按住的修饰键会永久卡在远端。触发点两个：窗口失焦
  （`on('windowEvent')` 的 `WINDOW_INACTIVE`/`WINDOW_HIDDEN`）与画面组件失焦（`XComponent.onBlur`）。
  客户端按**远端身份**记账仍按下的输入（按键=扫描码+扩展位 / Unicode 码点 / Win 替代键 `0x5B`，
  鼠标=`BUTTON` 标志），失焦时统一补发抬起并松开触屏与捏合；**新增输入通路必须进这份账**。

- **鼠标按键**：移动（`PTR_FLAGS_MOVE`）与按键事件**分开送**，按键事件**不带 MOVE 标志**，
  否则远端会忽略点击。
- **鼠标移动要合帧**：`onMouse` 的 Move 按设备轮询率到达（高回报率鼠标/触控板可达数百 Hz），逐条转发
  = 每条一个输入 PDU，且**链路越快背压越小、越拦不住这股流量**。故移动按
  `MOUSE_MOVE_MIN_INTERVAL_MS` 上限节流（约 125Hz）：只保留最新位置、必要时补一次尾帧，
  保证指针不会停在按下/抬起之外的位置；**任何非 Move 指针事件（按下/抬起）先冲掉待发移动**，
  否则点击会落在旧坐标上。这是**输入节流，不是显示帧率**。
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
  - **Ctrl+滚轮**（默认）：按 `1/(20×速度)` 线性映射为 **Ctrl+滚轮**（「缩放速度」），由远端应用
    自己缩放。
  - **触摸模拟**：在鼠标位置**合成两个原生触点**做真实双指缩放——距离 1:1、初始间距取窗口较短边
    的百分比（默认 8%，不写死分辨率）、按设置角度斜置。**不设计时器**，只在真实 `AxisAction.END`
    结束。
    - 起手 **30ms 内不发 MOTION**：避免 RDPEI 50Hz 合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时
      捏合会失效）。
    - ArkUI `AxisType` **没有旋转轴**，拿不到真实手指朝向（多指不上报手指信息），所以用角度设置顶替。
    - **捏合期间给一个定位光标**：合成了真实触点后，系统会把鼠标指针隐藏（触屏活动态），此时若开着
      「RDP 光标」就完全看不到捏合锚点。于是捏合期间（`touchPinchCursor`）把系统光标换成鸿蒙原生
      **手势手型**并强制可见：**张开手**（`HAND_OPEN`）= 手指张开/放大，**抓取手**（`HAND_GRABBING`）
      = 手指收拢/缩小；方向按手势比例判、带 2% 死区避免抖动来回切，只在方向真的翻转时才改一次样式
      （`setPointerStyleSync` 是系统调用，不能每帧调）。结束时恢复远端位图/隐藏态。
  - **直接缩放**：**不向远端发任何数据**，只在本机把画面放大，所以会话分辨率、码流与帧率都不变。
    以**光标为锚点**（其下的远端像素保持不动），**1.0 是拟合（letterbox）视图** ⇒ 下限 100%、上限
    8×，手势比例直接当倍数（不叠加「缩放速度」）。
    - 变换是给 XComponent **外层容器**的一个 ArkUI 矩阵（`matrix4`，以组件中心缩放 + 屏幕平移），
      纯合成层缩放、不碰像素：状态一变即上屏，静止画面也能看到，不依赖远端再出一帧。
    - **指针映射不变**：指针事件按组件的**未变换局部坐标**上报，`mapPointer` 拿到的仍是拟合视图里的
      点；锚点、基准矩形与它同取 `baseViewRect()`，三者不会漂移。
    - **不是 100% 即缩放模式**：此时所有滚动改成画面平移（触控板位移 1:1 跟随手指，鼠标滚轮每齿一个
      固定步长）且**不发远端**；回到 100% 恢复原滚动。平移与缩放共用同一套「盖满 / 居中」夹取，不会
      露黑边。
    - **收手吸附**：捏合落在 100%～108% 收手时用短缓出动画回落到正好 100%，免得一次很小的捏合把会话
      停在「差一点」的缩放态（那样滚动被本机吃掉）；更大则停在松手处。
    - 远端分辨率或窗口尺寸变化会重置回 100%（拟合矩形变了，旧变换会失配）。
- **触控板双指双击＝中键**（全局设置，默认关）：触控板对应用只上报单指点触＝左键、双指点触＝右键、
  双指滑动/捏合＝轴事件，**多指与指关节都不上报**，所以中键只能自己合成。开启后把触控板的双指轻触按
  **右键**延迟一个双击窗口发出：窗口内出现第二次双指轻触就改发**中键**（单次仍发右键），因此右键有约
  300ms 判定延迟；关闭则无延迟直发。合成点击一律按**一次按下＋抬起**整发（不会卡键）。
  - 只对触控板来源生效（判定见上「输入来源识别」），真鼠标右键不受影响。
- **自定义 Win 键映射**（全局设置，0＝关闭）：在 `SessionPage.handleKey` 里把指定 `keyCode` 改发 Meta。
  **真实 Win 键不转发**，避免"远端+本机"双重映射。
- **快捷键穿透**（全局设置 `shortcutPassthrough`，默认关）：系统级按键拦截
  （`OH_Input_AddKeyEventInterceptor`，需 ACL `ohos.permission.INTERCEPT_INPUT_EVENT`）接管键盘，
  让 Win / Alt+Tab / Esc 这些本会被 shell 吃掉的键也到远端。拦截事件按**本会话**路由，走与 `handleKey`
  相同的映射与记账通路；拦截期间 `handleKey` 不参与，避免双发。（与"自定义 Win 键映射"不同，穿透下
  **真实 Win 键会被转发**。）
  - 拦截是**进程级、且只在应用获焦时投递**，故只在**本会话窗口活动**且会话 `Connected` 时注册；其他窗口
    （设置页 / 主列表）变为活动、或本页离开即释放，否则那些窗口的键盘也会被吞掉。
  - 代价是开启期间**本机的系统快捷键一并被远端接管**，所以默认关；注册失败（缺 ACL 等）时原生返回
    `false`，UI 保持普通按键通路而不是把键盘丢掉。
- **锁定键（Caps / Num / Scroll Lock）以本机为准，RDP 跟随本机**：任何按键事件都读本机锁定态
  （`KeyEvent.isCapsLockOn` / `isNumLockOn` / `isScrollLockOn`，API 19；设备不给这几个字段时保持远端现状）
  并与已下发值比对，变了就发 RDP `Client Synchronize PDU`
  （`freerdp_input_send_synchronize_event`，位值与 `KBD_SYNC_*` 一致）。**非锁定键在转发前发**——这样该键的
  字符大小写已经正确，覆盖"焦点不在本窗口时本机锁定态被改"的情况；**锁定键本身在转发后发**——该键自己已经
  让远端翻转，先发会变成二次翻转。服务端的 `Set Keyboard Indicators` PDU（`update->SetKeyboardIndicators`）
  报远端锁定态，其客户端默认实现是**把 PDU 原样回发**，必须接管。**原生只把这个报告转成 `RemoteLockState`
  事件**（不做比对）；**与本地是否一致、要不要纠正由 ArkTS 决定**（`SessionPage.onRemoteLockState`：不一致
  且距上次重发 ≥1s 时调 `reassertLockState` 发一次**无去重**的 Client Synchronize，避免与服务端来回拉锯）。
  本机态由按键事件里的 `isCapsLockOn`/`isNumLockOn`/`isScrollLockOn` 记录（`localLockFlags`）。三个锁定键
  （Caps 0x3A、Scroll 0x46、Num 0x45）与各自的小键盘扫描码都要在 `KeyMapper` 里齐全，否则本机锁定键根本
  到不了远端。
- **远端光标同步**（全局设置，默认开）：RDP 只传光标**位图**（系统指针仅 `SYSPTR_NULL`/`SPTR_DEFAULT`），
  所以照搬位图、**不做类型映射**；HarmonyOS 会把自定义位图缩放到固定的系统光标大小，故**无需**按
  画面/位图尺寸自行缩放。原生侧用 `graphics_register_pointer` 接管并按需缩放（预乘 alpha 面积平均），
  UI 侧 `createPixelMapSync` + `setCustomCursorSync`；默认/隐藏走系统默认样式与可见性。
  关闭开关则不接管、回退默认箭头。（**模拟器无鼠标，只能真机验证**。）
  - **去重在 ArkTS**：原生把手上的每个形状都发过来（它只负责缩放/放大），UI 侧按"几何 + 像素"哈希
    （`cursorShapeHash`）丢掉指针缓存命中式的重复形状，只在真正变化时才重建 `PixelMap` 与系统光标；
    `Default`/`Hidden`/窗口重置都会让这个哈希失效。每个形状事件都带一个原生位图句柄，UI 必须 `bufferRead`
    后 `bufferRelease`（不接管光标时也要释放，否则句柄泄漏）。
  - **超分辨率下按同一倍率放大位图**：服务器按**会话分辨率**的像素尺度发光标位图，开了超分辨率就比画面
    小 `输出÷会话` 倍，所以原生按同一倍率放大再交给系统（`UpscaleBgra`）。放大是**本机 CPU 上的一次真
    重采样**——预乘 alpha 的 Lanczos3 + 对比自适应锐化（FSR 锐化阶段的思路），不是双线性拉伸。之所以
    不走 GPU 超分：位图最终要变成 `PixelMap` 交给系统光标，本来就得回到 CPU，走 GPU 只是多一次提交与
    回读，且光标的透明通道在 GPU 侧无法稳妥处理。**预乘 alpha 是必须的**：透明区的 RGB 是 0，直接对
    直通 alpha 的彩色做线性滤波会把黑混进边缘（黑边），所以先预乘、放大后再反预乘。**256 的系统上限仍然
    生效**：放大后超限就退回面积平均缩小（热点随之缩放）。注意这影响的是**位图尺度/观感**，不是屏幕上的
    光标大小（系统会归一到固定光标尺寸）。

## 3. 工具栏与状态遥测

- **显示规则**：全屏模式沿用**悬浮自动隐藏**（鼠标靠近屏幕顶部才滑出；触发/隐藏延迟在
  **开发者选项**里调）；
  窗口模式**始终显示**，且作为普通行布局在远程画面**上方**（渲染区自然扣除工具栏高度、不被覆盖，
  指针映射仍以 XComponent 局部坐标为准）。右侧按钮：复制 / 粘贴 / 全屏·退出全屏 / 最小化 / 断开。
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

- **帧工时计量（`hmrdp_gfx_work.{h,cpp}`，live 与回放共用）**：同一个 `GfxWorkMeter` 由**同一批钩子**
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
    `compose` 与 `present` 的划分、墙钟公式（`本机 + sync + presentWait`）与自检（`本机 + sync ≈ 1/fps`）
    见 [`gfx-engine.md`](gfx-engine.md) §7.1。
  - **节拍睡眠（`pace`）是唯一直接剔除的一项**：回放的人为节流不是客户端工作。
  - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值
    （只有服务端注册该回调），故连接时给 `context->autodetect` 自行注册回调捕获。
  - **带宽**：`freerdp_get_stats()` 的收发字节差分。**FPS**：只显示**应用呈现数**（对端帧率）；
    面板刷新率（恒为 60/120 一类）不进工具栏。
  - **音频**：采样率 + 近期**丢帧率**（欠载补静音 + 队列超上界时丢掉的整包；在"数据仍在到达"时统计，
    取滑动窗口——停顿不计，流真正结束后才停；缓冲与丢包策略见 [`native-libraries.md`](native-libraries.md) §5）。
  - **CPU**：`hidebug.getCpuUsage()` 的**进程**占用率（%，每次遥测采一次）。官方把这个模块定位为
    调试接口（相对耗性能），所以只在这条 dev 独占的遥测行里、按秒采一次；设备缺该能力时回调失败，
    这一项显示 `CPU --`。系统占用率（`getSystemCpuUsage`）**不显示**：这里要回答的是"本机应用花了多少"。
  - **GPU**：**本机自己的 GPU 上屏工时/帧**（ms），由呈现器的时间戳查询分成
    **上传 / 超分辨率 / letterbox** 三段（悬停提示给拆解）。它**不是 GPU 整机占用率**——平台没有对三方
    应用开放该接口，所以这里给的是"这一帧本机让 GPU 干了多久"，用来判断超分辨率本身的开销。
    **只有 Vulkan 呈现器能测**（GLES 兜底与无时间戳支持的设备回 0，显示 `GPU --`）。
  - **不显示**：服务端处理（协议不回报）、压缩比（只对 GDI 位图路径有意义）、音频丢包
     （复用在同一传输里，客户端无逐包统计）、**响应延时**（协议无回显、只能"空闲后输入→首帧"推断，
     噪声大，故不测量）。

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
  延迟类（网络/本机）统一用**毫秒、取整到个位**；本机的**拆相放在悬停提示**里
  （同样毫秒整数：解压+解析 / 解码 / 合成 / 上屏，后跟 `KB/帧` 与"本机工时占帧周期的百分比"）。
  `本机` 的告警阈值按"还能不能到 60/30fps"理解（16ms / 33ms）。
  GPU 一项给**一位小数**（本来就只有几毫秒，取整会看不出差别），拆解同样在悬停提示里。
  **字段顺序：固定字段在前（网络 / 本机 / FPS / CPU / GPU），会消失的字段按"出现频率从高到低"排在后面
  （带宽 - 有流量就显示 → 音频 - 播放时才显示）**：条件项越靠后，它出现/消失时能影响到的字段就越少
  （带宽的文字还随速率变宽，本身不定宽）。

## 4. 剪贴板（**手动触发**）

**同步在设计上就是用户手动触发的**：工具栏「复制」（远端→本机）与「粘贴」（本机→远端），
**刻意不做自动同步**，扩展图片/文件等类型时也必须沿用。理由：

1. 自动读本机剪贴板需要受限开放的读权限、有隐私成本；手动则由系统 `PasteButton` 安全控件在运行时临时授权，
   读取无需声明 `READ_PASTEBOARD`；
2. 图片/文件等大数据量只有显式触发才可控（二次确认、进度、目标路径）；
3. 两个方向逻辑对称（都是"读一侧 → 写另一侧"），扩展类型只需改中间那段转换。

手动读取不声明 `READ_PASTEBOARD`，但 `getData` / `getDataWithProgress` 仍会被编译器**静态**提示该权限
（授权是运行时由安全控件给的）；两个读取方法用 `@SuppressWarnings({rules:[SuppressWarningsType.PERMISSION]})`
压掉该告警。

**不要引入剪贴板自动监听或自动写本机剪贴板。** 原生覆写 cliprdr 的 `Server*` 回调与 `MonitorReady`，
只从 `Client*` 读/发它需要的表示。当前支持 **纯文本 / 富文本（HTML）/ 图片 / 文件**；文件优先于
图片 > HTML > RTF > 文本。两个按钮按缓存到的最丰富类型给差异化提示。

**原生只搬字节，"选哪个"在 ArkTS**（[`architecture.md`](architecture.md) §3.1）：`FORMAT_LIST` 到达时原生
按 wire 形式把受支持格式分类成候选（`kind:id`，同一 kind 只留一个，DIB 先于 DIBV5）并发 `RemoteFormatList`
事件；**优先级、单在途、延迟刷新都在 ArkTS**（`SessionPage.onRemoteFormatList` 按上表优先级选一种，经
`requestRemoteFormat` 下发）。数据响应不带格式 id ⇒ 同一时刻**只允许一个请求在途**；原生记下在途请求的
kind 并在响应上回带，另发一个 `RemoteFormatResponse`（无论成功/空/失败）作为"本次已结束"的信号，ArkTS
据此取它在这期间记下的最新待刷新项。`FORMAT_DATA_REQUEST`（服务端拉本机内容）是**同步 serve**，仍在原生。

### 4.1 文件传输

- 入口沿用两个按钮（不新增独立按钮）：远端剪贴板是文件列表时「复制」触发下载，本机剪贴板含文件 URI
  时「粘贴」触发上传。仍**手动触发**，不做自动或监听。
- **协议**：先传 `FileGroupDescriptorW`（u32 数量 + 592 字节 `FILEDESCRIPTORW` 列表），再按
  `lindex`/`streamId` 用 `CB_FILECONTENTS_REQUEST/RESPONSE` 流式传字节（`FILECONTENTS_SIZE` 取
  64 位大小、`FILECONTENTS_RANGE` 取块；描述符已带 `FD_FILESIZE` 时跳过 SIZE，与 Windows 客户端一致）。
  客户端能力位取 `CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS |
  CB_HUGE_FILE_SUPPORT_ENABLED`——**不要置 `CB_CAN_LOCK_CLIPDATA`**：一旦协商了锁，服务端会下发
  `clipDataId` 并要求**每个**文件内容请求回带，未回带的请求被 `CB_RESPONSE_FAIL` 拒绝，表现为
  「远程拒绝提供文件内容」、上传方向同样卡住；FreeRDP 自带的客户端文件传输也把这个位置注释掉
  （`client_cliprdr_file.c` 的 `cliprdr_file_context_current_flags`）。通道把客户端声明的标志与服务端
  能力**取交**（`cliprdr_main.c`），服务端不支持时自动降级。文件格式 id 在客户端注册范围自选
  （`0xC001`/`0xC002`），服务端按**格式名**识别（依赖已协商的 `CB_USE_LONG_FORMAT_NAMES`）。
  服务端下发的能力标志在 `HmrdpCliprdrServerCapabilities` 里记 hilog，用来核对取交结果。
- **字节的源/汇由 App 提供**：FreeRDP 的 client cliprdr 只实现线协议（分块、stream 状态、huge 校验、
  lock 转发），内容要 App 自己读写。分工按 [`architecture.md`](architecture.md) §3.1 的判据：**上传**是
  server 拉块、回调须在 RDP 线程**同步应答**，故留在原生（按请求从沙箱文件回块）；**下载**不是同步回调
  （只要求在回调期间把字节拷走），故状态机整个在 ArkTS——原生把每个 `FILECONTENTS_RESPONSE` 拷成
  `RemoteFileChunk` 事件交给 ArkTS，由它写沙箱文件、维护 offset/size 序列并发下一个请求
  （`requestRemoteFileContents`，一次一个在途）。文件块用独立的 `streamId` 序列。
- **平台侧**：本机→远端用系统安全控件授权 + `getDataWithProgress({destUri})`，由系统把剪贴板文件拷进
  应用沙箱（无需 `READ_PASTEBOARD`，也不必手工处理 URI 授权）；远端→本机先落沙箱，完成后用
  `fileUri.getUriFromPath` + `MIMETYPE_TEXT_URI` 写回本机剪贴板，其他应用粘贴时由框架按 URI 取。
  `getDataWithProgress` **不支持文件夹**，故不递归目录；非文件 URI（网页链接）走文本路径。
- **落地与失败**：下载目录按时间戳分桶避免覆盖；描述符未带 `FD_FILESIZE` 时才逐文件发
  `FILECONTENTS_SIZE`；任一文件失败即中止并清掉本次分片。**上传**进度由原生随同步 serve 上报
  （`kFileTransferProgress`，载荷 `方向|已完成|总量|第几个|共几个`）；**下载**进度在 ArkTS 本地按
  自身 done/total 计算，不跨边界。速率均由 UI 按相邻两次 `done` 差值算，不进原生。工具栏内联显示
  「第几个/共几个 + 百分比 + 速率 + 较宽的进度条 + 取消」——多文件时靠计数看出整体在推进，不单看进度条。
  取消分两种：
  **上传未开始拉取**（`localTransferDone_ == 0`）时是**干净撤回**——把本地剪贴板改广告成空文本
  （`CF_UNICODETEXT` + 2 字节 NUL），服务端据此丢掉文件格式，之后远端粘贴得到空文本而不是报错；
  **上传已开始拉取**时只能停服，远端粘贴会报协议错误，故这种情况先弹一次确认。**下载**在 ArkTS 侧
  直接停并清掉本次分片。
  下载完成由 ArkTS 写本机剪贴板、上传完成走 `kFileTransferDone`。
- **上传完成后必须撤回文件剪贴板**（`WithdrawLocalFileClipboard()`，即上面「未开始拉取」那一段逻辑）：
  广告出去的列表字节全部送完之后（`localTransferDone_ >= localTransferTotal_`，全零字节的文件在描述符响应里
  即算完成），远端若再粘贴一次会**重新拉同一批文件**，而此时 UI 的进度行与取消已随完成清空，表现成「没有进度条
  的重传」。故两处完成点在发 `kFileTransferDone` 之后立刻换成空文本广告——字节已取完，换内容不会打断本次拉取，
  服务端据此丢掉文件格式，之后再粘贴什么也拉不到。上传被取消时走同一段代码，不许分叉出两套清理。
- **上传是"广告"而非推送，不能预传**：cliprdr 的文件字节是**服务端拉取**模型，只有远端粘贴时服务端才发
  `FILECONTENTS_REQUEST`，客户端无法主动推，所以点「粘贴」后进度停在 0 直到对端粘贴。同理想"撤回"也
  只能靠换内容——空 `FORMAT_LIST` 会被通道直接丢弃（`cliprdr_main.c`：首次之后 `numFormats == 0`
  忽略）。故上传方向：点按钮后**在进度行原位**显示「在远端粘贴开始传输」提示（带转圈与取消），
  **拉取真正开始（`done > 0`）才换成进度行与取消**；不用 toast 提示（字数多且一闪而过看不清）。
- **不支持文件夹（两向）**：文件剪贴板只流文件。远端列表里出现任一目录即**整份拒绝**（不做部分下载）；
  本机侧选到文件夹同样拒绝。提示统一为「不支持文件夹传输」。混合列表情形因此不存在（有目录即拒绝）。
- **传输期间工具栏不自动隐藏**：只要有传输在跑，或上传还停在「在远端粘贴开始传输」，全屏工具栏就被
  钉住显示（不按悬停/延迟收起），让进度和提示始终可见；传输结束（成功/失败/取消）恢复正常的悬停显隐。
- **沙箱临时文件的清理口径**：每次传输新建 `<时间戳>` 目录，而剪贴板只引用**最新**那一个，所以新建时删除同
  root 下的旧目录（`rdp_inbox` / `rdp_outbox` 各自只保留一个），占用因此恒定，也不会破坏当前剪贴板里仍然
  有效的 URI；取消/失败时删掉本次已落盘的分片（下载侧在 ArkTS）。**不在启动或会话结束时清**——前者会让上个
  进程残留在剪贴板里的 URI 失效，后者在多窗口下会误删别的窗口刚写入的目录。
- **一次只允许一个文件传输**：两个按钮在 `transferBusy || transferActive || transferWaiting` 时直接拒绝
  并提示「正在传输文件，请先取消或等待完成」。否则两条状态机会挤在同一个进度行里，上传侧更糟——服务端正在
  按 `listIndex` 拉取时替换文件列表会让索引错位。**中止必通知**：远端 `FORMAT_LIST` 变化时原生发
  `kRemoteClipboardChanged`，ArkTS 据此中止自己正在跑的下载（清理分片并收起进度行），避免进度行等一个
  永远不会来的事件；点击复制时列表已空则直接提示。


### 4.2 实现坑（改剪贴板前必看）

- **大图剪贴板冻结画面（服务端侧，客户端不是瓶颈）**：把图片推到远端时若只广告未压缩 `CF_DIB`（整屏
  32bpp 可达数十 MB），**服务端**在接收/消化它期间停止发画面更新，表现为画面与输入冻结好几秒。客户端
  把该载荷交给通道仅数毫秒、RDP 线程在整段冻结里无长阻塞 ⇒ 瓶颈在服务端、不在客户端发送。**解法是
  减小载荷**：同时广告压缩的注册格式 `PNG`（置于 `CF_DIB` 之前，服务端会拉 PNG），载荷通常小一个量级，
  冻结随之消失；`CF_DIB` 保留为服务端不支持 PNG 时的兜底。文件不受影响：字节走 `FILECONTENTS` 分块。
- **远端格式协商**：`FORMAT_LIST` 的候选分类在原生（wire 形式），`文件(FileGroupDescriptorW) > 图片(DIB/DIBV5)
  > HTML Format > Rich Text Format > CF_UNICODETEXT` 的**优先级选择在 ArkTS**（见 §4 开头）。**数据响应不带
  格式 id** ⇒ 同一时刻**只允许一个请求在途**，期间的新列表记为待刷新，`RemoteFormatResponse` 回来再取；
  分派时**严格按请求时的 kind**，绝不用二进制兜底当文本（DIB 头 `28 00 00 00` 按 UTF-16 会变成 `(`）。
- **本机 HTML 读取**：不能只看 `getPrimaryHtml()`（只读第一条记录，且富文本常把 `text/plain` 作主 MIME、
  HTML 作附加 Entry）。要**遍历记录**用 `record.getData('text/html')` 取，再回退文本；
  不要用 `getMimeTypes()` 做门控（它可能只列主类型）。
- **HTML 只加壳、不改内容**：若源 HTML 已自带 `<!--StartFragment-->`/`<!--EndFragment-->`（Word/WPS
  导出都带）就**复用**，只算字节偏移（CF_HTML 偏移是**字节**，不是字符），不注入重复标记、不规范化
  原内容。写本机时若远端只给片段，用最小 `<html><body>` 包成良构文档；多格式必须落在**同一 Record
  的不同 Entry**，不能建两条 Record。
- **RTF 兜底**：很多 Windows 应用只显式提供 `Rich Text Format` 而无 `HTML Format`（HTML 是 Word 在 OLE
  层按需合成的，`EnumClipboardFormats` 枚举**不会触发**合成），故客户端自带 RTF→HTML 转换。
  注意 `\colortbl` 索引约定：第一个 `;` 是索引 0（auto），之后每个 `;` 递增，解析时**不要预置空条目**，
  否则 `\cfN` 整体错位、颜色丢失。
