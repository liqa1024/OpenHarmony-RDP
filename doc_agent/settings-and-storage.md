# 设置 / 存储 / 密码 / 导入导出

## 1. 存储分工

| 数据 | 位置 | 说明 |
|---|---|---|
| 全局设置 | `SettingsStore`（preferences） | 显示、连接特性默认值、窗口、输入等 |
| 连接配置 | `ConnectionStore`（preferences） | **稳定 UUID 作 id** + `updatedAt`（供 UI 刷新/作 key） |
| 密码 | `CredentialStore`（**ASSET**） | 按连接 id 存，**只在连接验证成功后写入** |
| 背景图片 | 应用沙箱 `files/background_<时间戳>.<ext>`（路径存 `SettingsStore`） | 主界面 / 设置页 / 应用页（后两页可覆盖），仅本机使用，不随配置导出 |
| 导出文件 | 用户选择位置（`DocumentViewPicker` + `fileIo`） | JSON，**不含密码** |

- **密码绝不落明文、不进日志、不经命令行传递**：原生侧用 `freerdp_settings_set_string` 设
  `FreeRDP_Password` / `FreeRDP_GatewayPassword`。
- preferences 的字符串字段**先 `encodeURIComponent` 再拼接**，否则控制字符会损坏文件。
- 连接可带一个**可选自定义名**（`SavedConnection.name`）：非空时
  列表主标题、删除确认与会话窗口标题都用它（`label()` / `displayName()`），空则回落到主机地址
  （`host`）。显示连接目标时（`label()` / `targetLabel()` / `subtitle()`）**默认端口不落进文案**——
  端口等于本协议默认值（RDP `3389` / Moonlight `47989`）只显示主机地址，非默认才拼 `host:port`。
  它只是显示名，**不参与任何连接参数**；随连接一起持久化（记录尾部追加一个字段，旧记录缺该字段即
  视为空）并随导入导出搬运。

### 1.1 系统密码保险箱（只做填充，保存走应用自身）

编辑页的用户名 / 密码框同时充当系统的「登录表单」，让系统密码保险箱能填充已保存的账号：

- 用户名框 `type(InputType.USER_NAME)`、密码框 `type(InputType.Password)`，两者**在同一个界面**
  才构成可识别的登录表单（判定口径见官方「输入框类型说明」与「系统可适配的场景」）。
  这是填充的全部条件，其余字段类型不影响。
- **保存不由应用发起，也无法由应用发起**：密码保险箱的保存是系统在页面跳转时自己判定的；
  编辑页在会话窗口打开的同时离开，系统不会弹保存提示，保险箱里也不会留下本应用的记录。
  `autoFillManager.requestAutoSave` 不解决这个问题——它落到的是**智能填充服务**（Scenario Fusion Kit）的
  「保存至历史表单输入」，该服务只对**已完成认证的企业开发者 + AGC 申请审批通过**的应用开放，调用一律被
  系统拒绝（`onFailure`），与是否上架无关。**不要为了密码去接它**：「历史表单输入」是明文表单历史，
  还会随华为账号云空间同步。
- 没有本应用记录时，系统的填充列表会推荐**其他应用/关联账号**——那是系统的跨应用推荐，
  不代表本应用保存过任何东西。
- `enableAutoFill` 绑定到页面状态，**离开编辑页时置 false**：填充只在停留编辑页期间有效，
  取消 / 返回 / 仅保存 / 删除都不会把未验证的密码交给系统去保存。
- 凭据保存仍走应用自身（见 §1、`CredentialStore`）：保险箱未启用、无锁屏密码或用户拒绝填充时，
  只是少一层系统级填充，不影响连接与重连取密码；两个控件退化为普通输入框。
- `InputType.USER_NAME` 无字符限制，`域`、`\`、`@` 等 RDP 用户名写法不受影响。

## 2. 设置模型：全局默认 + 单连接覆盖

- **显示（分辨率/缩放）**：全局可自动（分辨率取当前显示器，缩放按远端分辨率换算）或手动；
  单连接可打开「使用会话独立显示设置」，其中**分辨率与缩放各自也有 自动 / 手动**：自动分辨率取当前屏幕，
  自动缩放按该连接自己的分辨率换算（`conn.resolutionAuto` / `conn.scaleAuto`）；只有关掉自动时才用保存的
  `width/height/scalePercent`（旧记录没有这两个标志，等同于全 手动，行为不变）。默认关＝跟随全局；
  三个单连接覆盖项在 UI 上都是「独立」正向、默认关，存储字段是反相的 `useGlobal*`。
  连接前用 `SettingsStore.resolveDisplay(conn)` 解析。
  - `分辨率` 是**输出分辨率**（超分辨率后的目标）；超分辨率关闭时它同时就是会话分辨率。
  - `缩放` 是**会话（超分辨率前）桌面**的 DPI 系数。自动档 = **输出分辨率档位 ÷ 超分倍率**
    （`SettingsStore.autoScalePercent`；超分关闭时倍率按 1 计，即只由输出分辨率确定）：
    档位本身按**长边**线性换算 `长边 ÷ 1600 × 100`，再**吸附**到档位 `100/125/150/175/200/225`
    （长边 < 1600 即落到 100）。例（超分关闭）：`1600 → 100`、`1920 → 125`、`2560 → 150`、
    `2880 → 175`、`3200 → 200`、`3840 → 225`。**本机屏幕分辨率/密度不参与**——同一个远端分辨率
    在任何设备上换算出同一档位。
  - 原生只写 `FreeRDP_DesktopScaleFactor`（缩放交给服务端，客户端不做逐像素缩放）。
- **超分辨率**（RDP：`srBackend` + `srRatioPercent`；Moonlight：`mlSrBackend` + `mlSrRatioPercent`。
  两者都是各自的全局默认，**每个协议页签各存一份、都放在「显示」段**（分辨率与缩放之后——超分是分辨率
  链路的一环）；单连接用独立的「使用会话独立超分辨率设置」开关覆盖，
  默认关＝跟随本协议全局）：开启后
  **会话分辨率 = 分辨率设置 ÷ 倍率**（取偶像素），服务端按它出画，本机 Vulkan 呈现器再把它上采样回
  输出分辨率——省解码与带宽，边缘比双线性放大更锐。倍率档位 `SR_RATIO_PRESETS` =
  `1.25x`/`1.5x`/`1.75x`/`2x`：**平台只给建议区间**（空域 GPU 超分建议 `[1.2, 1.5]`），接口本身对倍率
  无硬限制，档位超过 `1.5x` 是拿画质换带宽（文字多的桌面在 `2x` 下边缘会发硬），所以倍率选项与开关在
  **同一栏**里，不另起一段。
  - **是三选一而不是开关**：关闭 / `XEngine` / `FSR`（`SR_BACKEND_XENGINE` / `SR_BACKEND_FSR`，持久化成
    0/1 并在连接记录尾部占一个字段）。两者都是「会话小、本机放大」，差别只在**用哪个上采样器**：
    XEngine 是平台空域超分（要设备特性），FSR 是呈现器自己跑的 FidelityFX Super Resolution 1.0
    （只要 Vulkan 上屏）。见 [`present.md`](present.md) §4。
  - **默认缩放随之换算**：自动档 = 分辨率档位 `÷ 倍率` 再吸附到档位（`SettingsStore.autoScalePercent`）；
    会话桌面小了、显示时又被放大回输出分辨率，系数按同一倍率缩小才能让远端 UI 保持同样的物理大小。
  - **前提是能力，不是开关**：Vulkan 上屏可用，且**所选后端**可用（FSR 只要前者，XEngine 还要设备报
    `XEG_spatial_upscale`）；**Moonlight 还多要一项**：设备能把解码器的 NV12 缓冲导入呈现器
    （`VK_OHOS_external_memory` + R8 线性图可创建，`superResolutionBackends().videoImport`）。设置页按后端
    逐个**置灰**并显示原因；不满足时按关闭处理。
    `SettingsStore.resolveSr(protocol, setting)` 是唯一的解析与兜底点（`conn` 两者都满足 `SrSetting`；
    `srBackendUsable(backend, protocol)` 把能力合起来判，选中的后端不可用时改用另一个，都不行才关掉），
    所以会话绝不会谈下一个本机放大不回去的分辨率。
  - **会话尺寸由 ArkTS 算一次并下行**：`SettingsStore.srSessionSize(输出, 倍率)`（`makeProfile` 里落到
    `DisplayProfile.sessionWidth/Height`）是唯一口径；会话启动时把 `streamWidth/streamHeight` 交给原生
    （RDP `RdpConnectOptions`、Moonlight `MoonlightStreamConfig`），**原生不重算**——`width`/`height` 恒为
    输出尺寸，两个尺寸之差即"要不要超分"，`srBackend` 只是机制，`srRatioPercent` 只用于会话标题标签。
  - 单连接可独立配置的理由：同一台机器走局域网可以不开，走 frp 转发时需要省带宽就开。
  - **Moonlight 侧的两处联动**：主机握手分辨率（`STREAM_CONFIGURATION` / `/launch` 的尺寸）与
    **fps 上限钳制、自动码率**都取**会话分辨率**（`MoonlightConnector.prepare`），并把同一对尺寸作为
    `streamWidth/streamHeight` 下行给原生会话——解码跑在会话分辨率上、主机也按会话分辨率编码，见
    [`moonlight-video.md`](moonlight-video.md) §2。
- **高级连接特性（音频、忽略证书…）**：默认值放全局 `AppSettings`；单连接保存自己的值 +
  `useGlobalAdvanced` 标志（**默认跟随全局**）。连接前用 `SettingsStore.resolveAdvanced(conn)` 解析。
  编辑页打开「使用会话独立高级设置」后才用本连接的独立开关；**保存时仍持久化独立值**，便于随时切回。
- **低延迟音频（Moonlight 专属，`mlLowLatencyAudio`，默认开）**：**全局一份、不做单连接覆盖**（与「音频重定向」
  同理）；无音频输出设备时置灰并把已存值纠正为关。语义与回落见 [`moonlight-native.md`](moonlight-native.md) §2.3。
- **会话缩放（Moonlight 专属，`mlScaleMode` + `mlScalePercent`）**：写**基地版 Sunshine 虚拟显示器**的 Windows
  显示缩放，语义与 RDP 的 `DesktopScaleFactor` 同构（把远端 UI 缩放系数交给主机）；**虚拟屏随会话创建/释放，
  故无需还原**。三选一：`自动`（默认，与 RDP 同一套换算 `autoScalePercent`：输出分辨率档位 ÷ 超分倍率
  再吸附档位）、`手动`（`SCALE_PRESETS` 六档，**无自定义输入**）、`不额外设置`（不向主机发任何缩放请求）。
  与 RDP 显示设置同构：
  全局一份 + 单连接覆盖（沿用 `useGlobalStream`）。**仅基地版 Sunshine（foundation-sunshine）有效**，
  其它主机（无 `/displays` / `/display-scale`）静默跳过，见 [`moonlight-connection.md`](moonlight-connection.md) §3。
- **剪贴板恒开**：不提供任何开关（全局或单连接）；原生无条件置 `FreeRDP_RedirectClipboard`。
  工具栏的「复制/粘贴」手动触发，见 [`clipboard.md`](clipboard.md)。
- **GFX 恒开**：关掉图形管线会让会话完全不可用，故不提供开关；`Connect` 里无条件置
  `FreeRDP_SupportGraphicsPipeline`。
- **上屏后端不用开关**：呈现器**按能力自动选**——设备呈现判定成立就用 Vulkan，否则回落 GLES
  （判据见 [`present.md`](present.md) §5）。**没有「硬件加速」设置项**：Vulkan 可用与否是设备事实，
  不是用户偏好，做成开关只会造出"开了也用不上"的档位。dev 回放页仍保留 `路线:CPU/硬件加速` 的**临时**切换
  （不落库，仅用于在同一份录像上 A/B 两个后端），会话开始时被能力再次归一（`SessionPage`）。
- **回到前台自动重连**（`reconnectOnForeground`，进程级，默认关）：会话窗口从后台回到前台时，
  若该会话已被系统断开，则自动重连一次。它**不是保活**——系统对后台应用先冻结网络资源（约 2s）、
  再释放并 abort socket（约 12s），且官方判定终端类会话（SSH/Mosh/SFTP）不属于合法的高优先级后台服务，
  普通三方应用无法用长时任务撑住"网络空载 / 低频心跳"；官方给同类应用的建议架构就是**客户端前台重连**。
  因此这里只兜"回来时发现已经断了"，不与后台管控对抗。行为与触发条件见
  [`session-layer.md`](session-layer.md) §5。
- **页面背景图片**（`backgroundImage` + `settingsBackgroundImage` / `appsBackgroundImage`，进程级，默认无）：
  通用设置「外观」选择图片后复制进应用沙箱（`files/background_<时间戳>.<ext>`，每次选择用新文件名以免
  `Image` 缓存旧图）。三页默认共用 `backgroundImage`；设置页与 Moonlight 应用页可分别覆盖（为空即回退）。
  它是本机资产：配置导出不写路径、
  导入保留本机当前背景；清除 / 恢复默认时删除文件。文件与 URI 处理见 `services/BackgroundImage`。
- **快捷键穿透**（`shortcutPassthrough`，进程级，默认关）：会话窗口聚焦时用系统按键拦截把 Win / Alt+Tab /
  Esc 等发给远端（需 ACL `ohos.permission.INTERCEPT_INPUT_EVENT`）。按会话窗口焦点实时注册/释放，
  行为与代价见 [`session-layer.md`](session-layer.md) §8。
- **开发者选项**（`developerMode`，进程级，默认关）：dev/实验功能的**总开关**。关着时以下入口都不存在：
  首页的「回放测试」；设置页的「抓取 GFX 码流（测试）」「触屏高刷新率（实验）」「工具栏触发停留时间」
  「工具栏触发区域宽度」「工具栏宽度」「工具栏隐藏延迟」「滚动：回退直译映射」；以及两协议会话工具栏的性能遥测（RDP：网络 / 本机 / FPS / 带宽 / 音频 / CPU / GPU；
  Moonlight：网络 / 主机 / 本机 / FPS / 码率；口径分别见 [`rdp-session.md`](rdp-session.md) §5 与
  [`moonlight-session.md`](moonlight-session.md) §2）。关掉它还**一并关闭**
  已开着的抓取，避免"入口被隐藏、后台还在跑"。它与其余设置一样随导入导出搬运（文件里是什么就还原什么）；
  抓取本身仍只在开发者选项开着时运行。
- **设置页记忆停留分栏**（`lastSettingsTab`）：设置页三栏（通用 / RDP / Moonlight）进入时回到上次
  停留的一栏；从未记录（`-1`）时落在「通用」。它只决定设置页的初始焦点，与各栏内容无关，
  随导入导出搬运（同 `lastNewProtocol`）。

## 3. 导入 / 导出（`ConfigTransfer`）

- 导出内容 = 全局设置 + 全部连接；**密码不导出**（在 ASSET 中），UI 必须提示"导入后需重新输入"。
- 导出用**独立的 DTO**（`ExportedConnection`）而不是直接序列化 `SavedConnection`：否则会把继承来的
  `password` / `gatewayPassword` 字段带出去。
- 导入按连接 **`id` 覆盖或新增**，并刷新 `updatedAt`（保证 UI key 变化）。入口在设置页底部。
- **导出 / 导入同一份字段集**：设置项一律取文件里的值（含开发者选项），只有与设备能力相关的
  （硬件加速、音频、超分）在本机提供不了时才回退（关掉 / 换后端），不整项丢弃——所以一份配置能
  跨能力不同的机器搬运，各自落到本机能用的值，且导入后直接开会话也不会用到本机不支持的能力。
  **单连接**的超分字段（`useGlobalSr`/`srEnabled`/`srRatioPercent`/`srBackend`）随连接一起导出导入，
  连接时还会过 `SettingsStore.resolveSr()` 兜底，所以挂到没有对应能力的设备上也不会真生效。
  单连接的 Moonlight 缩放（`mlScaleMode`/`mlScalePercent`）同样随连接搬运。
- 解析导出文件时统一 `JSON.parse(text) as Record<string, Object>` 后逐字段读取（原因见
  [`arkts-conventions.md`](arkts-conventions.md) §2）。
- Moonlight 的协议与 `ml*` 字段随连接搬运，旧配置文件缺这些字段时按 RDP 导入；详见
  [`moonlight-connection.md`](moonlight-connection.md) §2。

## 4. 生效时机（**改设置前必读**）

- 连接相关设置（分辨率/缩放、超分辨率、连接特性、触屏高刷新率、使用 RDP 光标等）在**连接建立时读取**：
  改动后需要**重开会话**才生效，已打开的会话不受影响。RDP 缩放在**连接时**交给服务端；Moonlight 会话缩放也在
  连接时应用（`launch` 成功后写主机虚拟屏），**断开不还原**（虚拟屏随会话释放）。
- 窗口尺寸类设置只在**窗口创建时**生效。
- 「回到前台自动重连」在**每次回到前台时重新读取**（不缓存在会话里），会话进行中改也即时生效。
- 「快捷键穿透」在**会话页加载时读取**，之后按窗口焦点与会话状态实时注册/释放，改设置后需**重开会话**生效。
- 「相对鼠标模式」与其「相对移动速度」滑块（**Moonlight → 光标模式栏下方**，`mlRelativeMouse` / `mlRelativeSpeed`）
  在**会话页加载时读取**：只改指针落点与增益，与主机能力位无关。改设置后需**重开会话**生效。
- 「滚动：回退直译映射」（**通用 → 开发者选项**，全局 `scrollEngineFallback`）在**会话页加载时读取**：
  它决定会话页是否挂上那个透明滚动组件（默认挂＝系统滚动引擎负责轴向滚动），改设置后需**重开会话**生效。
- 「总是使用系统滑动组件」（**Moonlight → 触控板**，`mlTouchpadScrollAlwaysEngine`，默认关）在**会话页加载时读取**，
  配合主机能力位决定触控板滚动落点（默认＝主机广告 `LI_FF_TOUCHPAD_FRAME_EVENTS` 就走触点通道，否则系统滚动组件 + 滚轮），
  改设置后需**重开会话**生效；`mlVmouseHintShown` 只是"基地版主机首次连接时提示过一次"的标记，不进导入导出。
- 回放/抓取这类 dev 开关在**下一次回放开始时**读取（切换路线会重启回放）。
