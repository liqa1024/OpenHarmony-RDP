# Moonlight（云串流）连接

本文件是 **Moonlight / Sunshine 串流**这条路的知识：协议建模、存储布局、连接列表与编辑/设置界面的约定、
图标约束。它与 RDP 那条路**并列**，共享连接列表、会话外壳、输入与上屏管线；差异集中在连接配置与串流参数。
RDP 侧的存储、显示、超分等不在本文件内，见 [`settings-and-storage.md`](settings-and-storage.md)、
[`present-pipeline.md`](present-pipeline.md)。**原生侧**（客户端库构建与共享库链接、桥接层、HTTP 控制层、客户端
身份与信任存储、配对加密原语，以及后续的会话 / 输入 / 上屏）见 [`moonlight-native.md`](moonlight-native.md)；
**协议编排与 XML 解析在 ArkTS**（见其 §4）。

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
- 追加字段：`protocol`、`mlDefaultAppId`、`mlDefaultAppName`、`mlHostId`、`useGlobalStream`、
  `mlResMode`、`mlWidth`、`mlHeight`、`mlFps`、`mlBitrateKbps`、`mlCodec`。
  （曾追加过的 `mlPaired` / `mlHdr` / `mlYuv444` / `mlAudioConfig` / `mlGamepad` 已退役：不再读写，
  其**槽位按位置保留**，以免改动后续字段的位置。）
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
- 覆盖项：分辨率模式（跟随屏幕 / 固定宽高）、帧率、码率、编码器。
- **保存时始终把当前编辑器里的独立值写回连接**（即使此刻跟随全局），便于随时切回，与 RDP 的单连接覆盖一致。

## 4. 连接列表与编辑页

**列表**

- 两种协议**混排在同一列表**，靠卡片图标区分（见 §6）。排序、状态、右键菜单、拖拽全部复用。
- Moonlight 行有两个圆钮，**「应用」在「连接」左侧**：**连接** = 用默认应用直接连接/断开；**应用** = 进入
  **主窗口内的应用管理界面**（不是弹窗，主窗口内容切过去）。RDP 行只有连接钮。连接中/已连接的 Moonlight 行禁用应用钮。
  - **应用管理界面**：列主机应用、显示每个应用**是否在运行**、可**关闭**在跑的应用；点应用卡片即"连上并打开会话"
    （应用已在运行且是同一个 → 恢复；否则开启并连）。**默认应用**在图标右上角的五角星上设置。
  - **页头走共享的紧凑 `HeaderBar`**（返回 + 标题「应用」+ 右侧 slot），连接名/地址放**右上角**；**刷新不再用文字**，
    而是右下角的通用悬浮圆钮（`components/FabButton` + `ic_refresh`），与列表页的「新建」FAB 同形。
  - **卡片只保留图标语义，不要右键菜单**：图标右上角的五角星（`ic_star`，默认时 `accent` 填充、否则 `text_primary`）
    设默认；**只有正在运行的应用**才在图标底排居中叠两个动作圆钮，且**按是否已有会话切换形态**（靠悬浮提示说明，
    这里不弹窗）：
    - **无会话**：`ic_play`(accent)「连接」+ `ic_stop`(danger)「关闭应用」。
    - **有会话**（`Connecting`/`Connected`/`Disconnecting`）：`ic_power`(text_primary)「断开连接」+
      `ic_stop`(danger)「关闭应用并断开」（`/cancel` + `SessionManager.disconnect`）。
  - **无会话时按现有逻辑；有会话时同一套动作直接落到会话上**——「连接」即断开后重连，无需二次确认弹窗。
  - 未运行的应用卡片点击 = **连接**；**当已有应用在跑时是「切换」**（卡片悬浮提示随之变化），因此**不置灰**。
    连接预检按主机当前运行的应用选择走法：**是同一个应用 → 恢复（`/resume`）**；**是别的应用 → 先结束再启动**。
    会话在跑时切换会先断开当前会话再连新应用。这一走法在**既有的连接/切换/默认应用/重试入口**上收敛，不另设入口。
  - 卡片就是 poster 本身，**不再有外框 / 卡片底**：只有一层按 Moonlight 标准 box art 的 **200:267** 宽高比
    （`moonlight-qt` 的 AppView 即此尺寸）圆角裁切位图（fetch 到的位图 `ImageFit.Cover`、占位用 `ic_apps` 居中），
    与取回的位图同形，`Cover` 不再因框形不符而裁切/偏移。
  - **名称不上图，只在悬浮提示里**：提示为 `连接到「名称」`，已有应用在跑时改为 `切换到「名称」`；卡片本身只承载图标语义。
  - 图标元素全部叠在 poster 上（系统材质不挂在 poster 上）：**右上角**星标设默认、**左上角**「运行中」徽标、
    **底排居中**动作圆钮。叠在图片上的控件（星标 / 动作钮）各用一个**圆形底**承载对比度：优先 `CONTROL`
    沉浸材质（`THIN`，跟随系统、不加赋色，材质自身那层磨砂就够分开 poster 底色）；系统没有材质能力时
    回落组件级磨砂 `backgroundBlurStyle(BlurStyle.COMPONENT_THIN)`（API 11+，与材质同款观感；此时不用
    材质，故不与材质层冲突）；图形色走主题 token 随深浅色。**星标的圆比动作钮小**（星标 `28` / 图形 `18`，
    动作钮 `36` / 图形 `22`）。
  - 卡片点击与卡片内的星标/动作钮互不串扰（各自的 `onClick` 命中自身）。
- 行右键菜单对 Moonlight 追加「设为默认应用」；**默认应用只在列表/应用界面设置，编辑页不再有该字段**。
- 副标题：Moonlight 显示默认应用名（未选择时显示「桌面」），RDP 显示账号。

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
  单独排在两钮**下一行**、占满整行，不再与它们并排。

## 5. 设置页的协议分栏

- 设置页顶部是 **RDP / Moonlight 两个 tab**（下划线样式，与编辑页一致）。
- RDP tab 放显示/连接特性；Moonlight tab 放串流默认值（§3 的全局默认部分）。
- **通用项**：触控板 / 键盘 / 会话窗口的设置在两个 tab 都出现，**每个协议各存一份值**（RDP 用原有设置键，
  Moonlight 用 `ml*` 副本，如 `mlTouchpadScrollSpeed`）；UI 是同一段代码，靠切换数据源复用，切换 tab 时
  先保存当前协议再加载另一协议。**主窗口（大小、自动隐藏）是应用级、只存一份**，不随协议区分。
- 应用级项（开发者选项、配置备份）**不随协议区分**，两个 tab 共享同一份。
- 「恢复默认设置」是导入导出下方的红色按钮，**只重置当前协议**（先弹出确认），不影响另一协议与已保存连接。

## 6. 图标约定

- **ArkUI 的 SVG 渲染不绘制 `<text>` 元素**：图标里的字符必须转成矢量路径（描边或填充），
  写 `<text>` 在网页预览能看到、在上屏里不会出现。
- RDP 卡片用原有「显示器 + 信号」图标（`ic_remote_desktop`）。
- Moonlight 卡片用 `ic_proto_moonlight`：紫色底 + 白色显示器轮廓 + **纯白 8 分格圆盘**，
  再用一枚偏移圆形成缺口（新月）。缺口与分割线用 `userSpaceOnUse` 的线性渐变填充，
  按用户坐标取色，因此与背景渐变**零色差、无接缝**（不需要 mask，也不必用黑底或灰阶）。
- Moonlight 行的「应用」按钮用网格图标（`ic_apps`）：它**随主题**（`resources/dark/media/` 同名覆盖为浅色描边）
  画在**次级按钮底**（`input_bg`）上，深浅色都有对比度。不要让它与卡片同底——那样看不出是按钮，且暗色下
  深色描边整片消失。
- **需要运行时换色的图标用 `fill` 画，不要用 `stroke`**：`Image.fillColor` 只覆盖 SVG 的**填充**，对描边无效。
  故星标 / 连接 / 断开 / 关闭（`ic_star` / `ic_play` / `ic_power` / `ic_stop`）一律纯 `fill` 路径，由页面按状态给
  accent / text_primary / danger；`ic_refresh` 画在 accent 圆钮上，直接用白色 `fill`。`ic_apps` 这类固定描边
  图标只能靠 `resources/dark/media/` 的深色变体随主题。

## 7. 实现边界

- **前端**：协议建模、存储、导入导出、连接列表、编辑页、设置页分栏、图标。
- **原生客户端库**：moonlight-common-c（连同它的 enet / nanors 子模块，后两者随本库静态编入）
  交叉编译为共享库 `libmoonlight-common-c.so`，与其它依赖一起落在 `entry/libs/<abi>/` 并被 `libhmrdp.so`
  链接。它是**构建必备输入**（同 FreeRDP，缺 `.so` 即构建失败），不是平台能力；`moonlightProbe`
  恒回 `"1"`，仅作"库确实链上了"的链接断言。原生**串流会话桥** `hmrdp_moonlight_session`
  （`LiStartConnection` + 输入转发）见 [`moonlight-native.md`](moonlight-native.md)。
- **控制层**：`ets/moonlight/` 的 `MoonlightCrypto`（配对加密原语）、`MoonlightIdentity`
  （自签名 X.509 + HUKS 信封私钥 + 逐主机 pin）、`MoonlightHttp`（`@ohos.net.http` 传输）、`MoonlightClient`
  （`serverinfo → pair → applist → launch`）与 `MoonlightXml` / `MoonlightModels`。串流前的预检由
  `services/MoonlightConnector.ets` 编排（`serverinfo →`（未配对则先 `pair`）`→ launch`，并装配原生串流配置），
  由 `MoonlightSessionBackend.prepare()` 在 `SessionManager` 已发布的 `Connecting` 状态下驱动，故列表行加载态与
  RDP 一致（见 [`session-and-input.md`](session-and-input.md) §1）。列表 / 编辑页 / 应用管理页的
  「连接」统一经 `services/MoonlightLauncher.ets` 构造请求（解析分辨率、应用 id、`filesDir` 与配对 PIN 弹窗）后
  交给 manager。
- **配对 = 依赖主窗口的轻量弹窗**：客户端生成 4 位 PIN，弹窗只**显示** PIN（用户在主机上输入，四个数字居中放大），
  配对期间不开任何会话窗口；**主机端输入完成即握手结束，弹窗自动关闭并继续**（无需手动确认），只有「取消」是用户动作；
  配对完成后才 launch 并开会话（见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md) 待定项）。
- **会话窗口与后端**：`pages/MoonlightSessionPage.ets`（XComponent 先绑 surface，再
  `SessionManager.beginStream` 启动）、`services/MoonlightSessionBackend.ets`（`prepare()` 跑控制层预检并装配
  `moonlightConfig`，`start()` 交给原生）、`services/SessionBackendFactory.ets` 与
  `utils/MoonlightKeyMapper.ets`；`SessionAbility` 按协议路由到该页。窗口在**预检通过后**打开、绑好 surface 再
  启动（`requiresWindowAtStart`，因为 `LiStartConnection` 需要渲染目标）。工具栏与 RDP 同形：标题用共享的
  `SessionTitle`（名称 + 分辨率 + 括号里的**协商解码格式**，见 [`ui-design.md`](ui-design.md) §3.6），复用同一套
  chrome 与自动隐藏判定（`components/SessionChrome`、`services/ToolbarAutoHide`），支持全屏 / 最小化；连接中的
  状态卡带约 1000ms 显示阈值，避免"多闪一下"。**停止会话会释放原生窗口，所以窗口内「重试」先重新绑定仍存活的
  XComponent surface 再重启**（此时会话必然已不在活动态，原生允许再次绑定）。
- **范围之外（待后续里程碑）**：音频（Opus，当前空渲染器关音频）；HDR、环绕、手柄输入等能力在就绪前**不在
  界面暴露**；端到端串流待连真 Sunshine 验证；超分链路（buffer 模式 + Vulkan 导入）属独立里程碑，
  仅真机验证（见 [`moonlight-native.md`](moonlight-native.md) §2）。
- 本文件描述**前端约定、ArkTS 协议建模与控制层**；原生侧（客户端库 / 解码 / 串流会话 / 输入）
  见 [`moonlight-native.md`](moonlight-native.md)，两协议共用的界面组件与设计语言见 [`ui-design.md`](ui-design.md)。
