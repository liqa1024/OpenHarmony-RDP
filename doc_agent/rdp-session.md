# RDP 会话 / 输入 / 遥测

本文件是 **RDP 路**的会话窗口细节、输入落地与工具栏遥测的家。两协议共享的会话模型 / 状态机 / 窗口与全屏 /
自动隐藏 / 输入策略 / 工具栏自动隐藏 / 光标安装器见 [`session-layer.md`](session-layer.md)；
FreeRDP 补丁、音频、能力探测见 [`rdp-native.md`](rdp-native.md)；GFX 与 CPU 链路账目见
[`rdp-gfx.md`](rdp-gfx.md)；上屏见 [`rdp-present.md`](rdp-present.md)；剪贴板 / 文件传输见
[`rdp-clipboard.md`](rdp-clipboard.md)。

## 1. 连接错误分类（FreeRDP）

- 连接有**超时兜底**；错误/断开原因经 `SessionManager.describeError` 分类（原生格式 `<错误码>|<消息>`）。
  - 码的高 16 位是 FreeRDP 错误类：`1` = **ERRINFO**（服务端 ErrorInfo/断开原因），`2` = **ERRCONNECT**
    （本地连接失败）。原生在断开时优先报服务端 ErrorInfo（经 `freerdp_error_info` 取回，如
    `ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION` = 被另一个连接接管），否则报客户端 last error；
    **用户主动断开**没有原因，payload 为空。
  - 断开弹窗据此区分"被其他连接接管 / 空闲超时 / 远端注销 / 服务端驱动或系统进程异常 / 网络中断"，
    而不是笼统的"连接已断开"；空 payload 才回落到 `连接已断开`。

## 2. 原生会话生命周期与终结

- 原生 `Session` 的生命周期**单向**（`kIdle→kStarting→kConnected→kStopping→kDestroyed`，一次尝试一个对象、
  不复用）：只有 RDP 线程释放 FreeRDP context（`Teardown`），其它线程（输入/剪贴板/gdi 回调）须先
  `AcquireContext()` 取租约、用完 `ReleaseContext()`，`Teardown` 等**在途租约归零**后才释放，并由
  `WaitDestroyed` 发布"已彻底回收"。**新代码碰 context 一律走这条路径，不要再裸读 `instance_`/`cliprdr_`**。
- **终结有界且可等待**：`stopSession()` 向 RDP 线程发停止信号后**等待会话真正释放**（有界；若等待租约归零
  超时，则**放弃（泄漏）该 context 而不是释放它**——宁可泄漏也不在活着的使用者脚下释放，并记警告；
  这条路径正常不会走到），释放完成后才允许新尝试（进行中尝试拒绝新尝试见
  [`session-layer.md`](session-layer.md) §5）。

## 3. 不可见时抑制服务端输出

- **不可见时主动抑制服务端输出**（MS-RDPBCGR Client Suppress Output，`gdi_send_suppress_output`）：不可见即发
  suppress，服务端停发画面更新、客户端丢弃 surface 命令，停止解码/合成/上屏；可见后解除，画面由服务端重发。
  服务端不支持该能力时只省本地工作、不省带宽。可见性口径见 [`session-layer.md`](session-layer.md) §6。

## 4. RDP 输入落地

共享策略见 [`session-layer.md`](session-layer.md) §8；这里只写 RDP 侧落地。

- **触屏触点**：RDPEI 侧兜住合帧后的触点状态序列合法性（首条必须 DOWN 等），见
  [`rdp-native.md`](rdp-native.md) §3。
- **触屏高刷新率**（全局设置）：选中 ArkTS 触屏合帧器的窗口——开 8ms（约 125Hz）、关 20ms（约 50Hz），
  两协议一致（见 [`session-layer.md`](session-layer.md) §8）。rdpei 只负责把喂进的触点状态组装成合法帧
  （见 [`rdp-native.md`](rdp-native.md) §3）。
- **触控板双指滚动**：`TouchpadWheel` 累加出的轮转量按 **9bit 二补码（120=1 齿、单事件 ≤0xFF 分片）** 发给
  RDP，横向发 `HWHEEL`，对齐 FreeRDP 的 SDL 实现。
- **直接缩放**（RDP 呈现器专有，Moonlight 不暴露）：**不向远端发任何数据**，只在本机把画面放大，所以会话
  分辨率、码流与帧率都不变。以**光标为锚点**（其下的远端像素保持不动），**1.0 是拟合（letterbox）视图** ⇒
  下限 100%、上限 8×，手势比例直接当倍数（不叠加「缩放速度」）。
  - 变换是给 XComponent **外层容器**的一个 ArkUI 矩阵（`matrix4`，以组件中心缩放 + 屏幕平移），纯合成层
    缩放、不碰像素：状态一变即上屏，静止画面也能看到，不依赖远端再出一帧。
  - **指针映射不变**：指针事件按画面的**未变换局部坐标**上报，画面组件即内容比例，坐标直接对应内容视图，
    `mapPointer` 据此归一化到远端；锚点与夹取同取该画面尺寸，三者不会漂移。
  - **不是 100% 即缩放模式**：此时所有滚动改成画面平移且**不发远端**；回到 100% 恢复原滚动。平移与缩放
    共用同一套「盖满 / 居中」夹取，不会露黑边。系统滚动引擎（默认开）**直接驱动这份平移**：轴事件仍归引擎，
    逐帧增量（vp = 触控板位移 ÷ 密度，矩阵平移再 × 密度 ⇒ **真 1:1 跟随手指**）落到会话页的 `panView`——
    跟手、惯性、打断就是引擎那一套，逐帧判定见 [`session-layer.md`](session-layer.md) §8。回退直译后平移退化为
    逐事件映射（触控板取轴值按 vp 原样、滚轮每齿一个固定步长），无惯性。
  - **收手吸附**：捏合落在 100%～108% 收手时用短缓出动画回落到正好 100%，让停位落在整数缩放态；更大
    则停留在松手处。
  - 远端分辨率或窗口尺寸变化会重置回 100%（画面尺寸变了，旧变换会失配）。
- **锁定键（Caps / Num / Scroll Lock）以本机为准，RDP 跟随本机**：任何按键事件都读本机锁定态
  （`KeyEvent.isCapsLockOn` / `isNumLockOn` / `isScrollLockOn`，API 19；设备不给这几个字段时保持远端现状）
  并与已下发值比对，变了就发 RDP `Client Synchronize PDU`（`freerdp_input_send_synchronize_event`，
  位值与 `KBD_SYNC_*` 一致）。**非锁定键在转发前发**——这样该键的字符大小写已经正确，覆盖"焦点不在本窗口时
  本机锁定态被改"的情况；**锁定键本身在转发后发**——该键自己已经让远端翻转，先发会变成二次翻转。服务端的
  `Set Keyboard Indicators` PDU（`update->SetKeyboardIndicators`）报远端锁定态，其客户端默认实现是
  **把 PDU 原样回发**，必须接管。**原生只把这个报告转成 `RemoteLockState` 事件**（不做比对）；
  **与本地是否一致、要不要纠正由 ArkTS 决定**（会话页 `onRemoteLockState`：不一致且距上次重发 ≥1s 时调
  `reassertLockState` 发一次**无去重**的 Client Synchronize，避免与服务端来回拉锯）。本机态由按键事件里的
  `isCapsLockOn`/`isNumLockOn`/`isScrollLockOn` 记录（`localLockFlags`）。三个锁定键（Caps 0x3A、Scroll 0x46、
  Num 0x45）与各自的小键盘扫描码都要在 `KeyMapper` 里齐全，否则本机锁定键根本到不了远端。

## 5. RDP 工具栏主行与状态遥测

- **主行的分辨率是会话（超分辨率前）分辨率**，不是设置里的输出分辨率；开了超分辨率时在后面补一个
  括号，**先是后端名再是倍率**（` (FSR 1.5x)` / ` (XEngine 1.5x)`，两者都取连接选项，见
  [`settings-and-storage.md`](settings-and-storage.md) §2）。会话首帧到达前该值按
  `SettingsStore.srSessionSize()` 预置，收到 `kResize` 后以服务端实际桌面为准。
- **帧工时计量（`core/hmrdp_gfx_work.{h,cpp}`，live 与回放共用）**：同一个 `GfxWorkMeter` 由**同一批钩子**
  喂数（抓取钩子取 chunk 到达时刻，在 ZGX 之前、**落盘之后**；链式包裹的 `SurfaceCommand` 计解码；
  包裹的 `EndFrame` 计合成 + 上屏），所以**同一份码流在 live 与回放里各相位定义完全相同**，可逐相、
  按载荷对照。计量器**只产子项、不存总数**：各相位语义不同（CPU 工作 / GPU 提交 / 阻塞等待），合适的和
  由展示端相加，`sync` 这类阻塞项因此天然不计入 live 的合计。
- **工具栏左侧遥测**每秒刷新一次：原生在 RDP 线程按 ~1s 采一个**原始样本**（`sessionTelemetry()`——
  该窗口的收发/帧增量与工作/音频/GPU 子项，源在采样时清零；采样留在 RDP 线程，故不会与管线拆除竞争），
  **聚合在 ArkTS**（`services/RdpTelemetry`）：它把样本按自身窗口时长/帧数换算成速率与每帧均值，
  原生不产任何展示用速率。字段：
  - **本机**（µs/帧）= 四个**工作**相位之和（`zgx+parse` / `decode` / `compose` / `present`）；**`sync` /
    `presentWait` 是阻塞子项**（分别等 GPU 放开主缓冲、等显示端），单列、都不入 `本机`。相位语义、
    拆相与墙钟公式（`本机 + sync + presentWait`）见 [`rdp-gfx.md`](rdp-gfx.md) §7.1。
  - **节拍睡眠（`pace`）是唯一直接剔除的一项**：回放的人为节流不是客户端工作。
  - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值（只有服务端注册该回调），
    故连接时给 `context->autodetect` 自行注册回调捕获。
  - **带宽**：`freerdp_get_stats()` 的收发字节差分。**FPS**：只显示**应用呈现数**（对端帧率）；
    面板刷新率（恒为 60/120 一类）不进工具栏。
  - **音频**：采样率 + 近期**丢帧率**（欠载补静音 + 队列超上界时丢掉的整包；在"数据仍在到达"时统计，
    取滑动窗口——停顿不计，流真正结束后才停；缓冲与丢包策略见 [`rdp-native.md`](rdp-native.md) §5）。
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
    `cpuKHz=` 记档（[`rdp-gfx.md`](rdp-gfx.md) §7.1）。
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
- 遥测行本身由共享的 `TelemetryRow`（`components/SessionChrome`）渲染，页面只产出 `TelemetryField[]`；
   协议差异只在字段集合与各自口径（Moonlight 的字段与口径见 [`moonlight-session.md`](moonlight-session.md) §2）。
