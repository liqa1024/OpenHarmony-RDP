# Moonlight（云串流）连接

本文件是 **Moonlight / Sunshine 串流**这条路的**前端连接与界面**知识：协议建模、存储布局、会话独立覆盖、
连接列表与编辑页、设置页分栏、图标约定。它与 RDP 那条路**并列**，共享连接列表、会话外壳、输入与上屏管线；
差异集中在连接配置与串流参数。**原生侧**（客户端库构建与共享库链接、桥接层、解码/上屏/协商、控制/配对层）
见 [`moonlight-native.md`](moonlight-native.md)、[`moonlight-video.md`](moonlight-video.md)、
[`moonlight-control.md`](moonlight-control.md)；**主机与应用**（发现 / 运行态 / 唤醒 / 应用管理页）见
[`moonlight-apps.md`](moonlight-apps.md)；**会话窗口与遥测**见 [`moonlight-session.md`](moonlight-session.md)；
RDP 侧的存储、显示、超分见 [`settings-and-storage.md`](settings-and-storage.md)、[`rdp-present.md`](rdp-present.md)。

## 1. 协议建模

- 一条连接用 `SavedConnection.protocol` 区分协议：`0` = RDP（`PROTOCOL_RDP`），`1` = Moonlight
  （`PROTOCOL_MOONLIGHT`）；缺省即 RDP，所以历史记录天然是 RDP。协议行为由连接子类承载
  （`RdpConnection` / `MoonlightConnection`，见 [`session-layer.md`](session-layer.md) §1）。
- 新建连接的默认协议是**上一次添加的协议**（`AppSettings.lastNewProtocol`，每次保存连接时更新）；
  新建流程直接以该协议打开编辑页。
- **协议只在新建时可选**：编辑已有连接时协议锁定，避免改协议后与已存字段语义冲突。

## 2. 存储布局

- 连接记录仍是 `ConnectionStore` 的**位置分隔字段**（见 [`settings-and-storage.md`](settings-and-storage.md) §1）。
  协议与 Moonlight 字段**追加在记录尾部**：旧记录在该位置之前就结束，反序列化时回落默认值，
  因此新增字段不改变既有字段的位置，也不影响老记录。
- 追加字段：`protocol`、`mlDefaultAppId`、`mlDefaultAppName`、`mlHostId`、`useGlobalStream`、
  `mlResMode`、`mlWidth`、`mlHeight`、`mlFps`、`mlBitrateKbps`、`mlCodec`、`mlBitrateAuto`、`mlColorSpace`、`mlMac`、
  `mlScaleMode`、`mlScalePercent`。
  （退役字段**按位置保留槽位**、不读写，以免改动后续字段的位置。）
- `mlMac` 是主机网卡 MAC（`serverinfo` 的 `mac`），**唯一**来自探测的主机地址，是唤醒魔术包的目标；
  探测成功时落库（见 [`moonlight-apps.md`](moonlight-apps.md) §2 的主机四态），此前为空。它属于旧主机，
  **改主机时一并清空**（与 `mlHostId` 同策）。
- **地址与端口复用 RDP 的 `host` / `port`**：Moonlight 不用账号/域/网关/密码字段。端口默认值是
  Moonlight 的 `47989`（RDP 为 `3389`），由编辑页按协议给出，存储上仍是同一个 `port`。
- **是否已配对以本地存的证书为准**：权威配对是客户端持有的证书，导入配置 / 主机重装等情况下的
  "已配对"不可信，配对必须在连接时重新判定；连接记录不保存配对状态。
- 全局默认值放 `AppSettings.ml*`（画质 + Moonlight 自己的一份通用设置，见 §5）；导出/导入
  （`ConfigTransfer`）覆盖协议与 `ml*` 字段，旧配置文件缺这些字段时按 RDP 导入。

## 3. 会话独立覆盖（与 RDP 同构）

- Moonlight 的画质项遵循与 RDP 显示/超分/高级相同的「**全局默认 + 单连接覆盖**」写法：
  `SavedConnection.useGlobalStream` 为真时跟随全局，为假时用本连接自己的 `ml*` 值。
- 解析入口是 `SettingsStore.resolveStream(conn)`（全局默认见 `SettingsStore.globalStreamProfile()`），
  与 `resolveDisplay` / `resolveAdvanced` 并列。
- 覆盖项：分辨率模式（跟随屏幕 / 固定宽高）、帧率、码率（自动 / 手动）、编码器、色彩空间（SDR：Rec709 / Rec601）、
  会话缩放（自动 / 手动 / 不额外设置）。
- **会话缩放（仅基地版 Sunshine）**：语义与 RDP 的 `DesktopScaleFactor` 同构——把远端 UI 的缩放系数交给主机，
  区别是这里**只写基地版内置的虚拟显示器**（friendly_name `Zako HDR`）：`launch` 成功后 `GET /displays` 找到它，
  再 `POST /display-scale` 落缩放。两条都是 **nvhttp 的 HTTPS（47984）**端点、要**已配对证书**，与本仓
  `serverinfo` / `applist` 同一条 mTLS 通道（`MoonlightClient.fetchVirtualDisplay` / `setDisplayScale`）。
  - **主机报的百分比是「档位序号」的错标，必须按序号回译**：`DISPLAYCONFIG_SET_DPI_SCALE` 的 `scale_rel` 是
    「相对推荐值的第几档」，主机把这个序号当作**自己那张档位表的下标**用。基地版那张表比 Windows 真实档位表长
    （多出 `120/140/160/180`，16 项 vs 12 项），于是 `get_display_scale_details` 上报的
    `supported_scale_percents` / `recommended_scale_percent` / `current_scale_percent` 从第 2 项起全部错标；
    `set_display_scale` 又按错表查索引，导致真实存在的百分比被判 `400 unsupported_scale`、或在区间内的值落到别的档
    （例：要 200 会落到顶层 300）。故客户端**先把待设百分比换算成 Windows 真表的排名**，再取**主机表里同排名的 label**
    发送（`MoonlightClient.hostScaleLabel`，真表见同处 `WINDOWS_SCALE_STEPS`）；主机那张表若修正，此步即恒等。
    日志记 `wanted → label → applied`。
  - `supported_scale_percents` 仍是**可设范围**（主机表长度 = 该显示器支持的档数）；超出范围时按排名 **clamp 到顶/底**。
  - **身份用两把键**：`/displays` 的 `device_id` 是带花括号的 GUID、`display_name` 是 `\\.\DISPLAYn`；请求两者都带
    （主机先按 `device_id` 匹配，落不到再按 `display_name`）。
  - 虚拟屏随会话创建/释放，**故不需要断开还原**。**原版 Sunshine 无这两个端点**（`/displays` 返回 404），
    非基地版主机、主机没启用虚拟屏、`scale_set_supported` 为假都在找不到 `Zako HDR` 时**静默跳过**，不影响连接。
  - 模式只在连接时读一次：`自动` 按请求分辨率用 RDP 同一套换算（`SettingsStore.scalePresetForResolution`）取档位，
    `手动` 用保存的六档值（`SCALE_PRESETS`，无自定义输入），`不额外设置` 不发请求。
- **码率与帧率的取值口径**：帧率默认 60，档位（30/60/90/120）+ 自定义输入（`mlFps`，`utils/MoonlightQuality.clampFps`）。
  码率照 RDP 分辨率/缩放的「自动 / 手动」写法——**自动**（`mlBitrateAuto`）按 `分辨率 × 帧率 × 编码` 换算（`recommendedBitrateKbps`，
  bits-per-pixel 启发式），**手动**用 `mlBitrateKbps`；**UI 一律以 Mbps 显示与输入、存储仍是 kbps**（`kbpsToMbps` / `mbpsToKbps`）。
  自动值在连接时按**最终解析出的分辨率**（跟随屏幕已解析）计算，见 `MoonlightConnector`。
- **保存时始终把当前编辑器里的独立值写回连接**（即使此刻跟随全局），便于随时切回，与 RDP 的单连接覆盖一致。

## 4. 连接列表与编辑页

**列表**

- 两种协议**混排在同一列表**，靠卡片图标区分（见 §6）。排序、状态、右键菜单、拖拽全部复用。
- Moonlight 行有两个圆钮，**「应用」在「连接」左侧**：**连接** = 用默认应用直接连接/断开；**应用** = 进入
  **主窗口内的应用管理界面**（不是弹窗，主窗口内容切过去）。RDP 行只有连接钮。**应用钮不随会话态禁用**：
  应用管理界面自己处理"已有会话"（有会话时动作落到会话上），会话运行中进入它是正常操作——界面细节见
  [`moonlight-apps.md`](moonlight-apps.md) §1。
- 连接钮的加载态归触发控件本身（与刷新 FAB 同一逻辑，见 [`ui-design.md`](ui-design.md) §4.2）。
- 行右键菜单对 Moonlight 追加「刷新」；**默认应用只在应用页的星标上设置，编辑页无该字段**。
- 副标题：Moonlight 显示默认应用名（未选择时显示「桌面」），RDP 显示账号。
- **行外观按可达性四态**（未知 / 服务未起画刷新钮、离线画唤醒钮，仅 Moonlight；判据与四态见
  [`moonlight-apps.md`](moonlight-apps.md) §2，外观见 [`ui-design.md`](ui-design.md) §3.5）。

**编辑页**

- 表头：左侧「取消 + 标题」，右侧协议 tab（下划线样式）；编辑已有连接时对端 tab 置暗、不可点。
- 在协议 tab 间切换时，**端口随协议默认值切换**（`3389` ↔ `47989`），除非用户已手改过端口。
- Moonlight 表单只保留连接必需字段：名称（可选）、主机名/IP、端口。默认应用不在此设置，而在**应用管理页的星标**上（见上）。
- **自动搜索主机**：名称栏位右侧的放大镜按钮启动通用 mDNS 搜索（`_nvstream._tcp` 的 DNS-SD 服务类型）。
  搜索期间**按钮图标就地变为加载态**（不弹加载弹窗，见 [`ui-design.md`](ui-design.md) §4.2）；有结果时弹出主机列表
  （主机名 + IP），点一行回填主机（名称为空时同时回填名称），不覆盖已输入的内容；**已是已保存连接的地址整行置灰、
  标注「已添加」且不可选**（与保存时的地址初筛同一口径）；无结果用 toast 提示"未发现主机…"。发现机制协议无关
  （见 [`architecture.md`](architecture.md) 的 `MdnsDiscovery`），但只有以 mDNS 广播的协议才搜得到
  （Windows 的 RDP 默认不广播）。
- **主机唯一**：一个主机只能有一条 Moonlight 连接。判重分**两层**：
  - **地址初筛**（保存 / 配对前）：按**归一化地址**（小写、去尾点，排除自身）比对已有连接，命中就拒绝——便宜，
    与搜索结果的「已添加」置灰同一口径，省掉一次没必要的连接尝试。
  - **`uniqueid` 兜底（权威）**：主机身份是 serverinfo 的 **`uniqueid`**（即 Moonlight 的 `NvComputer::uuid`），
    地址只是入口。拿到 `uniqueid` 的那一刻（配对 `pairOnly` / 连接预检 `prepare`，经 `MoonlightHostPolicy`）
    再判一次，命中别的连接就拒绝——这一层才认得出「同一台机器写成 IP / 主机名 / 不同大小写」。
  - **过时 / 失效清理**：保存**只**沿用已有 `mlHostId`；**改主机**时清空 `mlHostId` / `mlPaired` / 默认应用
    （均属旧主机）并解除旧主机配对；主机应答了**不同的 `uniqueid`**（重装 / 换机）视为失效，丢掉旧 pin 重新配对
    （`MoonlightConnector.invalidateStalePin`）。
- **配对随连接清理**：删除某条 Moonlight 连接（列表右键或编辑页），或编辑时**改了主机**，都会解除该主机的配对——
  **立即清本地 pin**（下次连接必须重新配对），并对主机 best-effort 发 `/unpair`（主机离线不阻塞本地清理，
  见 `MoonlightConnector.forgetHost`）。pin 的文件名对 host 归一化（小写、去尾点）。
- 画质项不放在主表单，而在**操作按钮下方的高级设置折叠区**里（与 RDP 一致）：一个
  「使用会话独立串流设置」开关，关闭时显示「全局：…」摘要，打开后才展开 §3 的覆盖项。
- 底部操作按钮：**保存**与主操作同级，主操作对 Moonlight 是**「配对」**（编辑页只完成配对，不打开会话；
  配对成功即保存并返回），**连接一律走列表页的连接钮**；RDP 的主操作仍是「连接」。**删除**（仅编辑已有连接时）
  单独排在两钮**下一行**、占满整行。

## 5. 设置页的分栏

- 设置页顶部是 **通用 / RDP / Moonlight 三个 tab**（下划线样式；编辑页仍只有两个协议 tab）。「通用」在最左。
- **协议 tab（RDP / Moonlight）** 与 RDP 对齐：「显示」在最上（RDP：使用RDP光标 + 分辨率 + 缩放；
  Moonlight：光标模式 + 分辨率 + 缩放），Moonlight 的「串流」段（帧率 + 自定义输入 / 码率自动或手动 / 编码器 /
  色彩空间）承载 §3 的其余全局默认；
  其后是共用节（触控板 / 键盘 / 会话）。这些共用节在两个协议 tab 都出现，**每个协议各存一份值**
  （RDP 用原有设置键，Moonlight 用 `ml*` 副本，如 `mlTouchpadScrollSpeed`）；UI 是同一段代码，靠切换
  数据源复用，切换 tab 时先保存当前 tab 再加载目标 tab。
- **「通用」tab 只放应用级、与协议无关的项**：窗口（主 / 会话窗口的尺寸、默认最大化、自动隐藏）、
  配置备份、开发者选项的**总开关**（连同工具栏触发 / 隐藏延迟这类协议无关的调参）。这些项**只存一份**，
  不随协议区分。
- **开发者选项里协议专属的工具落到对应协议 tab 底部**：总开关在「通用」，开启后 RDP 专属的工具
  （抓取 GFX 码流、触屏高刷新率）出现在 **RDP tab 的最下方**，不在「通用」里混排；Moonlight 专属工具按同一规则
  挂到 Moonlight tab。
- **边界按「是否硬性通用」划，不按功能名相近**：窗口类（主 / 会话窗口尺寸、全屏）与开发者选项是
  **应用级单份**，`SessionAbility` 建会话窗口时两协议取同一份；而显示分辨率、缩放、串流参数
  （帧率 / 码率 / 编码器）与触控板 / 键盘 / 前台重连属**流与交互层面，按协议各存一份**——两协议各有一套，
  不因名字相近就并成一份。
- 「恢复默认设置」只在「通用」tab，**一键把 RDP、Moonlight 与通用全部恢复默认**（已保存的连接不受影响）。
- Moonlight tab 的「显示」段有 **「光标模式」（`mlCursorMode`）三选一**：
  - **关闭**（`ML_CURSOR_OFF`）：不处理光标，画面里是主机画的光标，本机也画一个（可能双重）；
  - **隐藏系统光标**（`ML_CURSOR_HIDE_SYSTEM`，默认）：串流时在画面区域隐藏鸿蒙系统光标，让画面里的
    主机光标成为唯一光标（指针移到工具栏、以及非串流态时恢复）。对**任何**主机有效；
  - **同步系统光标**（`ML_CURSOR_HIDE_HOST`）：本地光标模式，主机不画光标、改发指针形状，本机装成系统
    光标（顺滑、无双重）。**只有基地版 Sunshine（foundation-sunshine）成立**，其它主机静默回退为「关闭」。
    设置页检测不到主机版本，所以该选项下**恒显示红字警告**（"仅在基地版 Sunshine 有效"）。
  - `setPointerVisibleSync` 是**进程级**，断开 / 退页必须恢复，否则主窗口也没有光标。协议细节见
    [`moonlight-native.md`](moonlight-native.md) §2.1；安装器、去重与可见性归属见
    [`session-layer.md`](session-layer.md) §10。
  - **「相对鼠标模式」（`mlRelativeMouse`，默认关）**只在选中「隐藏系统光标」时出现在同一栏下方（其它两档
    本机系统光标就是可见光标，位置必须与主机一一对应）：指针移动改发**相邻位置的差**（鼠标与触控板同一条逻辑），
    主机不开鼠标加速时手感与鸿蒙这边基本一致。开启后同一栏再出现**「相对移动速度」（`mlRelativeSpeed`，×，1.0 基准）**，
    用于在主机显示缩放之上做二次调整。落点见 [`moonlight-native.md`](moonlight-native.md) §2，
    依据与边界见 [`session-layer.md`](session-layer.md) §8。
- **音频开关两协议共用一份**：`AppSettings.enableAudio`（RDP tab「连接特性·音频重定向」的同一全局值）同时是 Moonlight 的
  音频主开关，Moonlight tab 的「音频重定向」拨钮编辑的是这同一个键（两个 tab 都持久化同一份值）；叠加 `DeviceCapabilities.audio()`
  置灰（无输出设备即关）。它派生 `MoonlightStreamConfig.enableAudio` 与 `/launch` 的 `localAudioPlayMode`
  （开=0 音频送客户端 / 关=1 主机本地播放），见 [`moonlight-native.md`](moonlight-native.md) §2.3。
- **低延迟音频开关（Moonlight tab，`mlLowLatencyAudio`，默认开）**：与「音频重定向」同在 Moonlight tab 末尾的「连接特性」分节，
  **仅在其开启时显示**（音频关时无从谈低延迟）。开 = 原生 OHAudio FAST 通路，关 = 与 RDP 同一条 ArkTS 播放链路。
  存储与置灰见 [`settings-and-storage.md`](settings-and-storage.md) §2，机制见 [`moonlight-native.md`](moonlight-native.md) §2.3。
- **每协议设置必须真正被消费**：给 Moonlight 加会话 / 输入行为时，**必须让对应的 `ml*` 生效**，
  不要在别处塞新内容而让 Moonlight 那份成为摆设。

## 6. 图标约定

- **连接动作图标**：连接与恢复共用 `ic_play`（仅悬停提示区分）；**「终止」（Moonlight，关会话 + 关主机应用）与
  「断开连接」（RDP，仅断本地会话）统一用 `ic_stop`**；应用页的「断开连接」用 `ic_disconnect`（断链）。
  三者都是纯 `fill` 路径，由页面按状态着色。
  **`ic_power` 只作离线行的「唤醒」钮**（电源语义），画在 `wake`（深绿）上、不用 danger——红底像"关机/破坏"动作；
  不可唤醒（无 mac / 本机不在主机网段）时画在 neutral 上并降不透明度（见 [`moonlight-apps.md`](moonlight-apps.md) §2）。**未知态的
  「刷新」钮**（未知 / 服务未起）用 `ic_refresh`，与列表页的刷新 FAB 同款（accent 圆钮 + 白色 `fill`）。
- **ArkUI 的 SVG 渲染不绘制 `<text>` 元素**：图标里的字符必须转成矢量路径（描边或填充），
  写 `<text>` 在网页预览能看到、在上屏里不会出现。
- RDP 卡片用原有「显示器 + 信号」图标（`ic_remote_desktop`）。
- Moonlight 卡片用 `ic_proto_moonlight`：紫色底 + 白色显示器轮廓 + **纯白 8 分格圆盘**，
  分割线用 `userSpaceOnUse` 的线性渐变填充，按用户坐标取色，因此与背景渐变**零色差、无接缝**
  （不需要 mask，也不必用黑底或灰阶）。
- Moonlight 行的「应用」按钮用网格图标（`ic_apps`）：该图标是**纯 `fill` 单路径**（四枚圆角方环，`evenodd`
  挖孔），画在**实心中性钮**（`neutral`，hover `neutral_hover`）上用白色 `fillColor`——实心钮风格与连接钮
  一致，但取**第三种颜色**（中性灰、Moonlight 的深色 chrome 调），既不与 accent（连接）抢、也不与 danger
  （终止）撞，更不与紫色的协议图标撞色。它**不**画在卡片同底上——那样看不出是按钮。
- **并列的图标要归一化「图形占 viewBox 的比例」，而不是在代码里各给一个尺寸**：同一个 `Image` 尺寸下，
  实际视觉大小由图形在 `viewBox` 里的占比决定。`ic_play` / `ic_stop` / `ic_power` 统一让图形占
  `viewBox` 约 58% 且居中：play 三角 `28×28`、stop 圆角方 `28×28`、power 用 `viewBox="-4 -4 32 32"`
  把原 18 单位的图形缩到约 `27px`。
- **纯三角按「重心」居中，不是包围盒居中**：`ic_play` 的包围盒中心在三角形重心右侧，按包围盒居中会显得
  偏左；取顶点 `M15 10 L43 24 L15 38 Z`，让**质心**落在 `viewBox` 中心（三角质心在底边上方 `宽/3` 处）。
- **主界面连接卡片的「应用」网格图标比连接三角放大一档**：这是对上方「按图形占比归一化」的**有意例外**——
  「应用」用 `24vp`（在卡片上需更醒目），而连接三角（`ic_play`）与「终止 / 断开」（`ic_stop`）保持 `22vp`。
  该例外只作用于主界面连接卡片（`ConnectionRow`），不改变应用页 tile 动作圆盘的尺寸（`MoonlightAppTile` 的 `ACTION_SIZE`）。
- **`ic_disconnect` 画成「断链」**：两个**开口**的圆角矩形链环，两环**成一夹角**（约 15~20°）而**不是共线**——
  共线摆成一条直线读不出「断开」；断口处用**两根短杠**表示链条断面，左上再放 3 根火花。
  环孔靠子路径反向绕线（`fill` 非零环绕）形成，整枚仍是**纯 `fill` 单路径**。它是对角线形、密度低，
  图形占 `viewBox` **约 73%**（大于 play/stop/power 的 58%）才与它们等观。
- **`ic_disconnect` 的线宽与圆角照 tile 的可读性定，不照抄参考图的细线**：tile 上的动作图标只有
  `ACTION_SIZE`（22vp），细线在那一档只剩约 1px、整枚读成灰糊；要加粗到**与未选中星标的实心同量级的视觉重量**，
  圆角也随线宽一起放大（圆角外径约为线宽的 1.5~2 倍），开口端停在圆角切点即可。
- **连接 / 断开 / 关闭复用同一套填充图标**（连接 `ic_play`、断开与关闭 `ic_stop`）：主窗口的连接钮和
  应用 tile 用同一套——连接钮画在**实心 accent / danger** 上，用 `fillColor('#FFFFFF')`（实心底上的白属
  允许的例外）；tile 画在材质圆盘上，按状态给 accent / text_primary / danger。**不要为同一语义另画一套
  描边图标**；`ic_disconnect` 是纯 `fill` 断链，与上条同一处理。
- **需要运行时换色的图标用 `fill` 画，不要用 `stroke`**：`Image.fillColor` 只覆盖 SVG 的**填充**，对描边无效。
  故星标 / 连接 / 断开·关闭 / 唤醒 / 应用（`ic_star` / `ic_play` / `ic_stop` / `ic_power` / `ic_apps`）一律纯 `fill`
  路径，由页面按状态给 accent / text_primary / danger / 白；`ic_refresh` 画在 accent 圆钮上，直接用白色 `fill`。
  需要随主题（且不换色）的描边图标才靠 `resources/dark/media/` 的深色变体。
