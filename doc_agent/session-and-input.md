# 会话窗口 / 输入 / 工具栏与遥测

## 1. 会话窗口模型

- 每个会话在**独立的 `SessionAbility`** 主窗口中打开（`launchType: specified` + 唯一 `instanceKey`）。
  必须用独立 Ability 才能拿到完整标题栏（子窗没有最小化按钮）。
- 每次连接生成 `sessionKey`；待连接参数放 `SessionRequests`，**Want 里只带 key（密码不进 Want）**。
- **主窗口后台连接、成功后才开窗**；失败只报错、不开窗。每个会话独占一个 `RdpNative` 实例。
- **关闭窗口即断连**：`SessionAbility.onWindowStageDestroy/onDestroy` → `SessionManager.release` +
  `SessionRequests.discard`。注意 `SessionPage.aboutToDisappear` 在窗口关闭时**不触发**，别依赖它断连。
- 连接有**超时兜底**；错误经 `SessionManager.describeError` 分类（原生格式 `<错误码>|<消息>`）。

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
- **触屏**（只认 `event.source === SourceType.TouchScreen`；手指 id `+1` 避开 FreeRDP 的 0 空槽）：
  - `handleMouse` 要过滤同源兼容鼠标事件，避免"一触两通路"产生杂点击；
  - 重复 `TouchType.Down` 忽略（补发 UP+DOWN 会变成"松开+重按"的假点击）；
  - `TouchType.Cancel` **不代表抬指**（move 会被打断）：挂起不抬指，若附近随后出现新的 Down/Move
    判定为同指续接触、只发 MOTION，超过 `CANCEL_HOLD_MS` 无续接触才真正 UP；
  - 压力按 `HAS_PRESSURE` 透传（`[0,65535)` → `[0,1024]`，0 表示设备未上报）。
- **触屏高刷新率**（全局设置）：FreeRDP `rdpei` 默认约 50Hz 合帧，是与桌面客户端的主要差距；
  补丁导出的运行时全局（弱符号引用）在开启时把间隔设为 0。未打补丁的 FreeRDP 上自动降级。
- **触控板双指滚动**（`sourceTool === TOUCHPAD`）：`axisVertical/Horizontal` 是本次事件的 **vp 位移**
  而非轮齿 ⇒ `services/TouchpadWheel.ets` 按 `120/16vp × 速度倍率` 累加成**高分辨率 RDP 轮转量**
  （9bit 二补码、120=1 齿、单事件 ≤0xFF 分片，对齐 FreeRDP 的 SDL 实现），横向发 `HWHEEL`。
  设置「滚动速度」只缩放轮转量、不改变发射粒度。
- **触控板捏合**：默认按 `1/(20×速度)` 线性映射为 **Ctrl+滚轮**（「缩放速度」）；设置
  「使用触摸模拟触控板捏合」改为在鼠标位置**合成两个原生触点**做真实双指缩放——距离 1:1、
  初始间距取窗口较短边的百分比（默认 4%，不写死分辨率）、按设置角度斜置。**不设计时器**，
  只在真实 `AxisAction.END` 结束。
  - 起手 **30ms 内不发 MOTION**：避免 RDPEI 50Hz 合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时
    捏合会失效）。
  - ArkUI `AxisType` **没有旋转轴**，拿不到真实手指朝向（多指不上报手指信息），所以用角度设置顶替。
- **自定义 Win 键映射**（全局设置，0＝关闭）：在 `SessionPage.handleKey` 里把指定 `keyCode` 改发 Meta。
  **真实 Win 键不转发**，避免"远端+本机"双重映射。
  - 窗口级"按键穿透"开关已删除（对系统保留键只能旁听、拦不住 shell，对普通键又无必要）；
    确需独占系统快捷键只能用系统级 `OH_Input_AddKeyEventInterceptor`/`AddKeyEventHook`（需 `system_basic`）。
- **远端光标同步**（全局设置，默认开）：RDP 只传光标**位图**（系统指针仅 `SYSPTR_NULL`/`SPTR_DEFAULT`），
  所以照搬位图、**不做类型映射**；HarmonyOS 会把自定义位图缩放到固定的系统光标大小，故**无需**按
  画面/位图尺寸自行缩放。原生侧用 `graphics_register_pointer` 接管并按需缩放（预乘 alpha 面积平均），
  UI 侧 `createPixelMapSync` + `setCustomCursorSync`；默认/隐藏走系统默认样式与可见性。
  关闭开关则不接管、回退默认箭头。（**模拟器无鼠标，只能真机验证**。）

## 3. 工具栏与状态遥测

- **显示规则**：全屏模式沿用**悬浮自动隐藏**（鼠标靠近屏幕顶部才滑出，延迟可调）；
  窗口模式**始终显示**，且作为普通行布局在远程画面**上方**（渲染区自然扣除工具栏高度、不再被覆盖，
  指针映射仍以 XComponent 局部坐标为准）。右侧按钮：复制 / 粘贴 / 全屏·退出全屏 / 最小化 / 断开。

- **帧工时计量（`hmrdp_gfx_work.{h,cpp}`，live 与回放共用）**：同一个 `GfxWorkMeter` 由**同一批钩子**
  喂数，因此**同一份码流在 live 与回放里"本机"的定义完全相同**，可以逐相、按载荷对照：
  抓取钩子取 chunk 到达时刻（在 ZGX 之前、**落盘之后**，磁盘 I/O 不进相位）、链式包裹的
  `SurfaceCommand` 计解码、包裹的 `EndFrame` 计合成 + 上屏。

- **工具栏左侧遥测**每秒刷新一次（原生 `kMetrics` 事件）。字段：
  - **本机**（µs/帧）= 客户端在这一帧上的全部**处理**时间：
    `zgx+parse`（chunk 到达 → 该帧第一条命令，即 ZGX 解压 + RDPGFX PDU 解析）+
    `decode`（包裹的 `SurfaceCommand`，含 progressive 自己的重复合成 `update_tiles`）+
    `compose`（`gdi_EndFrame` 的 surface→primary 合成，**再减去 present、该帧的 `sync` 等待与回放节拍
    睡眠**——这三样都在 `EndFrame` 窗口里但不是合成）+
    `present`（`PresentGdiFrame`，Vulkan/GLES 呈现器）。
  - **`sync`（单独一项，不在 `本机` 里）**：本帧在 `BeginPaint` 里等 GPU 放开它要写的桌面缓冲
    （CPU 路线把 gdi 主缓冲直接放在呈现器内存里，所以上一帧必须先读完）。它是**阻塞时间**、
    不是处理时间，故单列，这样工具栏上各拆相相加仍然等于 `本机`；
    **帧的整段墙钟 = `本机` + `sync`**（回放再加节拍睡眠）。轻样本跑满时这笔等待不可忽略
    （可达数 ms），别把"等 GPU"当成"合成贵"。
  - **节拍睡眠（`pace`）是唯一直接剔除的一项**：回放的人为节流不是客户端工作。
  - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值
    （只有服务端注册该回调），故连接时给 `context->autodetect` 自行注册回调捕获。
  - **响应**：RDP 输入与画面是两条**无回显**的流，输入延迟只能**推断**——仅在**空闲 ≥200ms 后输入、
    2s 内出现首帧**时采样，取近几次均值；不做该约束会退化成帧节拍。
  - **带宽**：`freerdp_get_stats()` 的收发字节差分。**FPS**：`<系统刷新率>/<应用呈现数>`。
  - **音频**：采样率 + 近期**丢帧率**（欠载补静音 + 环形缓冲溢出字节；仅活跃时统计，取滑动窗口，
    避免空闲静音误报）。
  - **不显示**：服务端处理（协议不回报）、压缩比（只对 GDI 位图路径有意义）、音频丢包
    （复用在同一传输里，客户端无逐包统计）。

- **口径约束（读数前必看）**：
  - **分母是 EndFrame 帧数**，不是呈现帧数：一次处理的工作在**它的帧收尾时**才入账，所以"一帧多大"
    （由到达节拍决定）不会跑偏分母；窗口边界最多切到一帧。
  - **拆相随内容变**：`decode` 随该帧载荷/命令数变（低 fps 时每帧扛的是累积变化，比高 fps 时大），
    `compose`/`present` 随桌面尺寸变 ⇒ 单看合计会把它误当成"客户端常量能力"。
    `bytesPerFrame`、`cmdsPerFrame` 与 **dutyPermille**（本机工时 / 墙钟）才是跨帧率可比的量：
    duty 远小于 100% 就是客户端在等数据。
  - **仍不含**：传输层读/drdynvc 重组（在抓取钩子之前）、**RDPGFX 帧回执**（FreeRDP 在 EndFrame
    回调返回**之后**才写）；`present` 里的 `WaitForFences`/`acquireNextImageKHR` 在合成器不还 buffer
    时会阻塞，排队/显示延迟会算进 `present`（与 `sync` 不同）。
  - 同一行每秒还有 hilog：`perf: 本机 X us/frame (max M) = zgx+parse … + decode … + compose … +
    present … (frames=… presents=… cmds/frame=… kB/frame=… duty=…%)`，定位用这一行。

- **显示口径**：指标用**固定宽度**排布（避免数字位数变化时重排），间距分"网络↔本机"与其余两档。
  延迟类（网络/本机/响应）统一用**毫秒、取整到个位**；本机的**拆相放在悬停提示**里
  （同样毫秒整数：解压+解析 / 解码 / 合成 / 上屏，后跟 `KB/帧` 与"本机工时占帧周期的百分比"）。
  `本机` 的告警阈值按"还能不能到 60/30fps"理解（16ms / 33ms）。

## 4. 剪贴板（**手动触发**）

**同步在设计上就是用户手动触发的**：工具栏「复制」（远端→本机）与「粘贴」（本机→远端），
**刻意不做自动同步**，扩展图片/文件等类型时也必须沿用。理由：

1. 自动读本机剪贴板需要受限开放的读权限、有隐私成本；手动则可用系统 `PasteButton` 安全控件临时授权，
   无需声明任何权限；
2. 图片/文件等大数据量只有显式触发才可控（二次确认、进度、目标路径）；
3. 两个方向逻辑对称（都是"读一侧 → 写另一侧"），扩展类型只需改中间那段转换。

**不要引入剪贴板自动监听或自动写本机剪贴板。** 实现上原生只覆写 `Server*` 回调与 `MonitorReady`，
不动 `Client*` 发送函数。当前支持 **纯文本 / 富文本（HTML）/ 图片**；文件传输预留。
两个按钮按实际内容类型给差异化提示。

### 4.1 实现坑（改剪贴板前必看）

- **远端格式协商**：`FORMAT_LIST` 按 `图片(DIB/DIBV5) > HTML Format > Rich Text Format >
  CF_UNICODETEXT` 优先级**选一种**请求。**数据响应不带格式 id** ⇒ 同一时刻**只允许一个请求在途**，
  期间的新列表记为待刷新，响应回来再取；分派时**严格按请求时的 kind**，绝不用二进制兜底当文本
  （DIB 头 `28 00 00 00` 按 UTF-16 会变成 `(`）。
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
