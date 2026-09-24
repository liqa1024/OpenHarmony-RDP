# 会话窗口 / 输入 / 工具栏与遥测

## 1. 会话窗口模型

- 每个会话在**独立的 `SessionAbility`** 主窗口中打开（`launchType: specified` + 唯一 `instanceKey`）。
  必须用独立 Ability 才能拿到完整标题栏（子窗没有最小化按钮）。
- 每次连接生成 `sessionKey`；待连接参数放 `SessionRequests`，**Want 里只带 key（密码不进 Want）**。
- **主窗口后台连接、成功后才开窗**；失败只报错、不开窗。每个会话独占一个 `RdpNative` 实例。
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
  列表再次点击据此发起新尝试；失败只产生 `kError`（不再同时发 `kDisconnected`），失败原因不会被
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
- **触控板捏合**：默认按 `1/(20×速度)` 线性映射为 **Ctrl+滚轮**（「缩放速度」）；设置
  「使用触摸模拟触控板捏合」改为在鼠标位置**合成两个原生触点**做真实双指缩放——距离 1:1、
  初始间距取窗口较短边的百分比（默认 8%，不写死分辨率）、按设置角度斜置。**不设计时器**，
  只在真实 `AxisAction.END` 结束。
  - 起手 **30ms 内不发 MOTION**：避免 RDPEI 50Hz 合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时
    捏合会失效）。
  - ArkUI `AxisType` **没有旋转轴**，拿不到真实手指朝向（多指不上报手指信息），所以用角度设置顶替。
  - **捏合期间给一个定位光标**：合成了真实触点后，系统会把鼠标指针隐藏（触屏活动态），此时若开着
    「RDP 光标」就完全看不到捏合锚点。于是捏合期间（`touchPinchCursor`）把系统光标换成鸿蒙原生
    **手势手型**并强制可见：**张开手**（`HAND_OPEN`）= 手指张开/放大，**抓取手**（`HAND_GRABBING`）
    = 手指收拢/缩小；方向按手势比例判、带 2% 死区避免抖动来回切，只在方向真的翻转时才改一次样式
    （`setPointerStyleSync` 是系统调用，不能每帧调）。结束时恢复远端位图/隐藏态。
- **自定义 Win 键映射**（全局设置，0＝关闭）：在 `SessionPage.handleKey` 里把指定 `keyCode` 改发 Meta。
  **真实 Win 键不转发**，避免"远端+本机"双重映射。
  - 窗口级"按键穿透"开关已删除（对系统保留键只能旁听、拦不住 shell，对普通键又无必要）；
    确需独占系统快捷键只能用系统级 `OH_Input_AddKeyEventInterceptor`/`AddKeyEventHook`（需 `system_basic`）。
- **远端光标同步**（全局设置，默认开）：RDP 只传光标**位图**（系统指针仅 `SYSPTR_NULL`/`SPTR_DEFAULT`），
  所以照搬位图、**不做类型映射**；HarmonyOS 会把自定义位图缩放到固定的系统光标大小，故**无需**按
  画面/位图尺寸自行缩放。原生侧用 `graphics_register_pointer` 接管并按需缩放（预乘 alpha 面积平均），
  UI 侧 `createPixelMapSync` + `setCustomCursorSync`；默认/隐藏走系统默认样式与可见性。
  关闭开关则不接管、回退默认箭头。（**模拟器无鼠标，只能真机验证**。）
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
  窗口模式**始终显示**，且作为普通行布局在远程画面**上方**（渲染区自然扣除工具栏高度、不再被覆盖，
  指针映射仍以 XComponent 局部坐标为准）。右侧按钮：复制 / 粘贴 / 全屏·退出全屏 / 最小化 / 断开。
  开发者选项开启时，遥测（网络 / 本机 / FPS / 带宽 / 音频）另起**第二行**，主行布局与非开发者模式
  **完全一致**（工具栏变高，`toolbarHeight` 随之增长，全屏自动隐藏的判定边界跟着用这个值）。
- **主行的分辨率是会话（超分辨率前）分辨率**，不是设置里的输出分辨率；开了超分辨率时在后面补一个
  括号，**先是后端名再是倍率**（` (FSR 1.5x)` / ` (XEngine 1.5x)`，`SessionPage.srIndicator()`，两者都取
  连接选项，见 [`settings-and-storage.md`](settings-and-storage.md) §2）。会话首帧到达前该值按
  `SettingsStore.srSessionSize()` 预置，收到 `kResize` 后以服务端实际桌面为准。

- **帧工时计量（`hmrdp_gfx_work.{h,cpp}`，live 与回放共用）**：同一个 `GfxWorkMeter` 由**同一批钩子**
  喂数，因此**同一份码流在 live 与回放里各相位的定义完全相同**，可以逐相、按载荷对照：
  抓取钩子取 chunk 到达时刻（在 ZGX 之前、**落盘之后**，磁盘 I/O 不进相位）、链式包裹的
  `SurfaceCommand` 计解码、包裹的 `EndFrame` 计合成 + 上屏。
  **计量器只产子项、不存"总工时"**：各相位的语义不同（CPU 工作 / GPU 提交 / 阻塞等待），合适的和取决于
  谁来读，所以由展示端相加。`sync` 这类阻塞项因此天然不会被计入 live 的合计。

- **工具栏左侧遥测**每秒刷新一次（原生 `kMetrics` 事件）。字段：
  - **本机**（µs/帧）= 展示端把四个**工作**相位相加：
    `zgx+parse`（chunk 到达 → 该帧第一条命令，即 ZGX 解压 + RDPGFX PDU 解析）+
    `decode`（包裹的 `SurfaceCommand`，含 progressive 自己的重复合成 `update_tiles`）+
    `compose`（`gdi_EndFrame` 的 surface→primary 合成，**再减去 present、该帧的 `sync` 等待与回放节拍
    睡眠**——这三样都在 `EndFrame` 窗口里但不是合成）+
    `present`（`PresentGdiFrame`，Vulkan/GLES 呈现器）。
  - **`sync`（阻塞子项，不在 `本机` 里）**：本帧在 `BeginPaint` 里等 GPU 放开它要写的桌面缓冲
    （CPU 路线把 gdi 主缓冲直接放在呈现器内存里，所以上一帧必须先读完）。
  - **`presentWait`（阻塞子项，不在 `本机` 里）**：`present` 那一段里**等显示端**的部分
    （Vulkan 的 `vkAcquireNextImageKHR` + fence 等待；GLES 的 `eglSwapBuffers`）。`present` 只记
    录制/上传/提交这类客户端动作，两者相加才是这一帧 present 的整段墙钟。
  - 两个阻塞子项都是**显示/GPU 侧的反压**，不是处理时间，故都在合计之外；
    **帧的整段墙钟 = 四相之和 + `sync` + `presentWait`**（回放再加节拍睡眠）。轻样本跑满时这笔等待
    不可忽略（可达数 ms），别把"等 GPU / 等显示"当成"合成贵 / 上屏贵"。
  - **节拍睡眠（`pace`）是唯一直接剔除的一项**：回放的人为节流不是客户端工作。
  - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值
    （只有服务端注册该回调），故连接时给 `context->autodetect` 自行注册回调捕获。
  - **带宽**：`freerdp_get_stats()` 的收发字节差分。**FPS**：只显示**应用呈现数**（对端帧率）；
    面板刷新率（恒为 60/120 一类）不进工具栏。
  - **音频**：采样率 + 近期**丢帧率**（欠载补静音 + 溢出丢掉的整包字节；在"播放已开始且数据仍在到达"
    时统计，取滑动窗口——停顿当场就计，只有流真正结束后才停，起播预缓冲不算丢；缓冲与丢包策略见
    [`native-libraries.md`](native-libraries.md) §5）。
  - **CPU**：`hidebug.getCpuUsage()` 的**进程**占用率（%，每次遥测采一次）。官方把这个模块定位为
    调试接口（相对耗性能），所以只在这条 dev 独占的遥测行里、按秒采一次；设备缺该能力时回调失败，
    这一项显示 `CPU --`。系统占用率（`getSystemCpuUsage`）**不显示**：这里要回答的是"本机应用花了多少"。
  - **GPU**：**本机自己的 GPU 上屏工时/帧**（ms），由呈现器的时间戳查询分成
    **上传 / 超分辨率 / letterbox** 三段（悬停提示给拆解）。它**不是 GPU 整机占用率**——平台没有对三方
    应用开放该接口，所以这里给的是"这一帧我们让 GPU 干了多久"，用来判断超分辨率本身的开销。
    **只有 Vulkan 呈现器能测**（GLES 兜底与无时间戳支持的设备回 0，显示 `GPU --`）。
  - **不显示**：服务端处理（协议不回报）、压缩比（只对 GDI 位图路径有意义）、音频丢包
    （复用在同一传输里，客户端无逐包统计）、**响应延时**（协议无回显、只能"空闲后输入→首帧"推断，
    噪声大；推断逻辑连同 `kMetrics` 字段一起删掉了，不再测量）。

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
    `cpuKHz=` 记档（[`gfx-engine.md`](gfx-engine.md) §8.3）。
  - **仍不含**：传输层读/drdynvc 重组（在抓取钩子之前）、**RDPGFX 帧回执**（FreeRDP 在 EndFrame
    回调返回**之后**才写）；`present` 里的 `WaitForFences`/`acquireNextImageKHR` 在合成器不还 buffer
    时会阻塞，排队/显示延迟会算进 `present`（与 `sync` 不同）。
  - 同一行每秒还有 hilog：`perf: work X us/frame = zgx+parse … + decode … + compose … + present …
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

1. 自动读本机剪贴板需要受限开放的读权限、有隐私成本；手动则可用系统 `PasteButton` 安全控件临时授权，
   无需声明任何权限；
2. 图片/文件等大数据量只有显式触发才可控（二次确认、进度、目标路径）；
3. 两个方向逻辑对称（都是"读一侧 → 写另一侧"），扩展类型只需改中间那段转换。

**不要引入剪贴板自动监听或自动写本机剪贴板。** 原生覆写 cliprdr 的 `Server*` 回调与 `MonitorReady`，
只从 `Client*` 读/发它需要的表示。当前支持 **纯文本 / 富文本（HTML）/ 图片 / 文件**；文件优先于
图片 > HTML > RTF > 文本。两个按钮按缓存到的最丰富类型给差异化提示。

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
  lock 转发），内容要 App 自己读写 —— 上传从沙箱文件按请求回块，下载把块追加到沙箱文件。
  `FORMAT_DATA_REQUEST` 仍一次只允许一个在途；文件块用独立的 `streamId` 序列。
- **平台侧**：本机→远端用系统安全控件授权 + `getDataWithProgress({destUri})`，由系统把剪贴板文件拷进
  应用沙箱（无需 `READ_PASTEBOARD`，也不必手工处理 URI 授权）；远端→本机先落沙箱，完成后用
  `fileUri.getUriFromPath` + `MIMETYPE_TEXT_URI` 写回本机剪贴板，其他应用粘贴时由框架按 URI 取。
  `getDataWithProgress` **不支持文件夹**，故不递归目录；非文件 URI（网页链接）走文本路径。
- **落地与失败**：下载目录按时间戳分桶避免覆盖；描述符未带 `FD_FILESIZE` 时才逐文件发
  `FILECONTENTS_SIZE`；任一文件失败即中止并清掉本次分片。进度经 `kFileTransferProgress` 上报
  （速率由 UI 按相邻两次 `done` 差值算，不进原生；`kFileTransferProgress` 载荷是
  `方向|已完成|总量|第几个|共几个`），工具栏内联显示「第几个/共几个 + 百分比 + 速率 + 较宽的进度条 +
  取消」——多文件时靠计数看出整体在推进，不单看进度条。取消分两种：
  **未开始拉取**（`localTransferDone_ == 0`）时是**干净撤回**——把本地剪贴板改广告成空文本
  （`CF_UNICODETEXT` + 2 字节 NUL），服务端据此丢掉文件格式，之后远端粘贴得到空文本而不是报错；
  **已开始拉取**时只能停服，远端粘贴会报协议错误，故这种情况先弹一次确认。下载两态都可直接取消。
  下载完成走 `kClipboardFilesReady`、上传完成走 `kFileTransferDone`。
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
- **不传文件夹**：文件剪贴板只能流文件。远端描述符里带 `FILE_ATTRIBUTE_DIRECTORY` 的条目**只计数、
  不下载**（`kClipboardFileList` 带 dirs 字段），但**必须留在列表里**——`FILECONTENTS_REQUEST.listIndex`
  指的是服务端广告的位置，过滤掉目录会让后续所有索引错位。本机侧 `getDataWithProgress` 本身不支持文件夹
  拷贝（返回的 URI 是目录就表示没拷到东西）。两向都在遇到目录时给显式提示：远端只有目录→「暂不支持文件夹
  传输」，与本机文件夹同理；混合时下载文件并在完成提示里带上跳过的目录数。
- **传输期间工具栏不自动隐藏**：只要有传输在跑，或上传还停在「在远端粘贴开始传输」，全屏工具栏就被
  钉住显示（不再按悬停/延迟收起），让进度和提示始终可见；传输结束（成功/失败/取消）恢复正常的悬停显隐。
- **沙箱临时文件的清理口径**：每次传输新建 `<时间戳>` 目录，而剪贴板只引用**最新**那一个，所以新建时删除同
  root 下的旧目录（`rdp_inbox` / `rdp_outbox` 各自只保留一个），占用因此恒定，也不会破坏当前剪贴板里仍然
  有效的 URI；取消/失败时原生再 `remove()` 掉本次已落盘的分片。**不在启动或会话结束时清**——前者会让上个
  进程残留在剪贴板里的 URI 失效，后者在多窗口下会误删别的窗口刚写入的目录。
- **一次只允许一个文件传输**：两个按钮在 `transferBusy || transferActive || transferWaiting` 时直接拒绝
  并提示「正在传输文件，请先取消或等待完成」。否则两条状态机会挤在同一个进度行里，上传侧更糟——服务端正在
  按 `listIndex` 拉取时替换文件列表会让索引错位。原生侧配套"中止必通知"：远端 `FORMAT_LIST` 变化导致下载
  被重置、或点击复制时列表已空，都发 `kFileTransferFailed`，避免 UI 进度行等一个永远不会来的事件。


### 4.2 实现坑（改剪贴板前必看）

- **远端格式协商**：`FORMAT_LIST` 按 `文件(FileGroupDescriptorW) > 图片(DIB/DIBV5) > HTML Format >
  Rich Text Format > CF_UNICODETEXT` 优先级**选一种**请求。**数据响应不带格式 id** ⇒ 同一时刻
  **只允许一个请求在途**，期间的新列表记为待刷新，响应回来再取；分派时**严格按请求时的 kind**，
  绝不用二进制兜底当文本（DIB 头 `28 00 00 00` 按 UTF-16 会变成 `(`）。
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
