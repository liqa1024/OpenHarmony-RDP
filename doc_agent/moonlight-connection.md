# Moonlight（云串流）连接

本文件是 **Moonlight / Sunshine 串流**这条路的知识：协议建模、存储布局、连接列表与编辑/设置界面的约定、
图标约束。它与 RDP 那条路**并列**，共享连接列表、会话外壳、输入与上屏管线；差异集中在连接配置与串流参数。
RDP 侧的存储、显示、超分等不在本文件内，见 [`settings-and-storage.md`](settings-and-storage.md)、
[`present-pipeline.md`](present-pipeline.md)。**原生侧**（客户端库构建与共享库链接、桥接层、HTTP 控制层、客户端
身份与信任存储、配对加密原语，以及后续的会话 / 输入 / 上屏）见 [`moonlight-native.md`](moonlight-native.md)；
**协议编排与 XML 解析在 ArkTS**（见其 §4.2）。

## 1. 协议建模

- 一条连接用 `SavedConnection.protocol` 区分协议：`0` = RDP（`PROTOCOL_RDP`），`1` = Moonlight
  （`PROTOCOL_MOONLIGHT`）；缺省即 RDP，所以历史记录天然是 RDP。
- 新建连接的默认协议是**上一次添加的协议**（`AppSettings.lastNewProtocol`，每次保存连接时更新）；
  新建流程不再弹协议选择框，直接以该协议打开编辑页。
- **协议只在新建时可选**：编辑已有连接时协议锁定，避免改协议后与已存字段语义冲突。

## 2. 存储布局

- 连接记录仍是 `ConnectionStore` 的**位置分隔字段**（见 [`settings-and-storage.md`](settings-and-storage.md) §1）。
  协议与 Moonlight 字段**追加在记录尾部**：旧记录在该位置之前就结束，反序列化时回落默认值，
  因此新增字段不改变既有字段的位置，也不影响老记录。
- 追加字段：`protocol`、`mlDefaultAppId`、`mlDefaultAppName`、`mlPaired`、`useGlobalStream`、
  `mlResMode`、`mlWidth`、`mlHeight`、`mlFps`、`mlBitrateKbps`、`mlHdr`、`mlCodec`、`mlYuv444`、
  `mlAudioConfig`、`mlGamepad`。
- **地址与端口复用 RDP 的 `host` / `port`**：Moonlight 不用账号/域/网关/密码字段。端口默认值是
  Moonlight 的 `47989`（RDP 为 `3389`），由编辑页按协议给出，存储上仍是同一个 `port`。
- `mlPaired` 只是 UI 用的**配对状态提示**：权威配对是原生客户端持有的证书，所以导入配置、主机重装等情况
  下可能出现"标记已配对但实际未配对"，配对必须在连接时重新判定（不依赖该字段）。
- 全局默认值放 `AppSettings.ml*`（画质 + Moonlight 自己的一份通用设置，见 §5）；导出/导入
  （`ConfigTransfer`）覆盖协议与全部 `ml*` 字段，旧配置文件缺这些字段时按 RDP 导入。

## 3. 会话独立覆盖（与 RDP 同构）

- Moonlight 的画质项遵循与 RDP 显示/超分/高级相同的「**全局默认 + 单连接覆盖**」写法：
  `SavedConnection.useGlobalStream` 为真时跟随全局，为假时用本连接自己的 `ml*` 值。
- 解析入口是 `SettingsStore.resolveStream(conn)`（全局默认见 `SettingsStore.globalStreamProfile()`），
  与 `resolveDisplay` / `resolveAdvanced` 并列。
- 覆盖项：分辨率模式（跟随窗口 / 跟随屏幕 / 固定宽高）、帧率、码率、HDR、编码器、YUV444、
  音频通道、手柄。
- **保存时始终把当前编辑器里的独立值写回连接**（即使此刻跟随全局），便于随时切回，与 RDP 的单连接覆盖一致。

## 4. 连接列表与编辑页

**列表**

- 两种协议**混排在同一列表**，靠卡片图标区分（见 §6）。排序、状态、右键菜单、拖拽全部复用。
- Moonlight 行有两个圆钮，**「应用」在「连接」左侧**：**连接** = 用默认应用直接连接/断开；**应用** = 进入
  **主窗口内的应用管理界面**（不是弹窗，主窗口内容切过去）。RDP 行只有连接钮。连接中/已连接的 Moonlight 行禁用应用钮。
- **应用管理界面**：列主机应用、显示每个应用**是否在运行**、可**关闭**在跑的应用；点应用即"连上并打开会话"
  （应用已开 → 直接连；未开 → 开启并连，两种点击操作一致）。**默认应用**也在这里设置。
  - 断开两态：**只断开会话（应用保留）** / **关闭应用并断开**。
- 行右键菜单对 Moonlight 追加「设为默认应用」；**默认应用只在列表/应用界面设置，编辑页不再有该字段**。
- 副标题：Moonlight 显示默认应用名（未选择时显示「桌面」），RDP 显示账号。

**编辑页**

- 表头：左侧「取消 + 标题」，右侧协议 tab（下划线样式）；编辑已有连接时对端 tab 置暗、不可点。
- 在协议 tab 间切换时，**端口随协议默认值切换**（`3389` ↔ `47989`），除非用户已手改过端口。
- Moonlight 表单只保留连接必需字段：名称（可选）、主机名/IP、端口、默认应用；默认应用在未配对时不可选。
- 画质项不放在主表单，而在**操作按钮下方的高级设置折叠区**里（与 RDP 一致）：一个
  「使用会话独立串流设置」开关，关闭时显示「全局：…」摘要，打开后才展开 §3 的覆盖项。
- 底部操作按钮：**保存**（只落配置并返回）与**连接**同级；编辑时另有删除。

## 5. 设置页的协议分栏

- 设置页顶部是 **RDP / Moonlight 两个 tab**（下划线样式，与编辑页一致）。
- RDP tab 放显示/连接特性；Moonlight tab 放串流默认值（§3 的全局默认部分）。
- **通用项（触控板、键盘、会话、窗口）在两个 tab 都出现，但每个协议各存一份值**：RDP 用原有设置键，
  Moonlight 用 `ml*` 副本（如 `mlTouchpadScrollSpeed`）。UI 是同一段代码，靠切换数据源复用，
  不复制界面；切换 tab 时先保存当前协议再加载另一协议。
- 应用级项（开发者选项、配置备份）**不随协议区分**，两个 tab 共享同一份。
- 「恢复默认设置」是导入导出下方的红色按钮，**只重置当前协议**（先弹出确认），不影响另一协议与已保存连接。

## 6. 图标约定

- **ArkUI 的 SVG 渲染不绘制 `<text>` 元素**：图标里的字符必须转成矢量路径（描边或填充），
  写 `<text>` 在网页预览能看到、在上屏里不会出现。
- RDP 卡片用原有「显示器 + 信号」图标（`ic_remote_desktop`）。
- Moonlight 卡片用 `ic_proto_moonlight`：紫色底 + 白色显示器轮廓 + **纯白 8 分格圆盘**，
  再用一枚偏移圆形成缺口（新月）。缺口与分割线用 `userSpaceOnUse` 的线性渐变填充，
  按用户坐标取色，因此与背景渐变**零色差、无接缝**（不需要 mask，也不必用黑底或灰阶）。
- Moonlight 行的「应用」按钮用网格图标（`ic_apps`），深色描边画在浅色底上才可见。

## 7. 当前实现边界

- **前端**已就绪：协议建模、存储、导入导出、连接列表、编辑页、设置页分栏、图标。
- **原生客户端库已接入**：moonlight-common-c（连同它的 enet / nanors 子模块，后两者随本库静态编入）
  交叉编译为共享库 `libmoonlight-common-c.so`，与其它依赖一起落在 `entry/libs/<abi>/` 并被 `libhmrdp.so`
  链接。它是**构建必备输入**（同 FreeRDP，缺 `.so` 即构建失败），不是平台能力；`moonlightProbe`
  恒回 `"1"`，仅作"库确实链上了"的链接断言。原生**串流会话桥** `hmrdp_moonlight_session`
  （`LiStartConnection` + 输入转发）也已落地。构建流程、共享库约定与目录见
  [`moonlight-native.md`](moonlight-native.md)。
- **控制层已接入连接流程**：`ets/moonlight/` 的 `MoonlightCrypto`（配对加密原语）、`MoonlightIdentity`
  （自签名 X.509 + HUKS 信封私钥 + 逐主机 pin）、`MoonlightHttp`（`@ohos.net.http` 传输）、`MoonlightClient`
  （`serverinfo → pair → applist → launch`）与 `MoonlightXml` / `MoonlightModels`。串流前的预检由
  `services/MoonlightConnector.ets` 统一编排：`serverinfo →（未配对则先 `pair`）→ launch`，并装配原生串流配置。
  列表与编辑页的「连接」都走它。
- **配对 = 依赖主窗口的轻量弹窗**：客户端生成 4 位 PIN，弹窗只**显示** PIN（用户在主机上输入），配对期间
  不开任何会话窗口；确认后再 launch 并开会话（见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md) 待定项）。
- **会话窗口与后端已接入**：`pages/MoonlightSessionPage.ets`（XComponent 先绑 surface，再
  `SessionManager.beginStream` 启动）、`services/MoonlightSessionBackend.ets`（读
  `PendingConnection.moonlightConfig`）、`services/SessionBackendFactory.ets` 与 `utils/MoonlightKeyMapper.ets`；
  `SessionAbility` 按协议路由到该页。窗口是**先开窗再启动**（`requiresWindowAtStart`），因为 `LiStartConnection`
  需要渲染目标。会话工具栏复用与 RDP 同一套 chrome 与自动隐藏判定（`components/SessionChrome`、
  `services/ToolbarAutoHide`），支持全屏 / 最小化。
- **尚未接入**：主机发现（先手动添加）、音频（Opus，当前空渲染器关音频）与 HDR / YUV444 的协商暴露；
  端到端串流待连真 Sunshine 验证。
- 本文件描述**已落地的前端约定、ArkTS 协议建模与控制层**；原生侧（客户端库 / 解码 / 串流会话 / 输入）
  见 [`moonlight-native.md`](moonlight-native.md)，两协议共用的界面组件与设计语言见 [`ui-design.md`](ui-design.md)。
