# UI 设计规范（视觉语言 / 布局模式 / 组件契约）

本文件是界面的**设计语言总纲**：设计原则、设计 token、布局模式、组件契约与共享工具。
它只回答"界面长什么样、由哪些组件组合、参数怎么给"，**不描述行为与数据语义**：

- 会话窗口 / 输入 / 工具栏行为 → [`session-layer.md`](session-layer.md)；RDP 会话 / 遥测 → [`rdp-session.md`](rdp-session.md)；
- 剪贴板与文件传输 → [`rdp-clipboard.md`](rdp-clipboard.md)；
- 设置项 / 连接存储 / 生效时机 → [`settings-and-storage.md`](settings-and-storage.md)；
- Moonlight 协议差异（字段、分栏、图标语义） → [`moonlight-connection.md`](moonlight-connection.md)；
- ArkTS / ArkUI 语法、状态刷新、主题资源规则、沉浸材质 API 与摆放机制 → [`arkts-conventions.md`](arkts-conventions.md)。

## 设计原则（先读）

规范思路先于下面的具体取值；凡"加共享单元 / 抽组件 / 新增控件"都按这几条办：

1. **共享按逻辑分类，不按当下使用**：抽取边界是**逻辑类别**（同一类卡片 / 页头 / chip / 窗口能力 / 自动隐藏……），
   不是"当前哪一侧在用"。属同一逻辑类的能力**一并抽出**，即使暂时只有一侧使用——这正是保留扩展性的方式
   （另一侧要用时直接取用，而不是再抄一份）。有协议独有但同类的能力同样抽出，由页面选择用不用。
2. **抽取必须是"可选能力"**：共享单元按需（opt-in）使用——不主动订阅事件、不申请能力，避免"为了通用"让
   窗口 / 页面背上用不到的行为。**范围可以大，但不能有未被调用的副作用。**
3. **同类控件同形同参**：同一语义的 chip / 卡片 / 页头 / 状态卡只允许一种实现，差异用参数表达，不复制粘贴。
4. **协议差异不复制界面**：优先"同一组件 + 协议参数"，其次才是协议各自的分支 builder；差异只体现在参数取值与图标资源上。
5. **颜色只走 token**（会话工具栏的固定深色除外，见 §1.2）；不以硬编码 hex 表达主题语义。
6. **尺寸 / 圆角 / 字号 / 阴影 / 动画收敛到常量或组件默认值**，不在页面里各写一份。
7. **材质挂载遵守表面清单与摆放约束**（§2 + [`arkts-conventions.md`](arkts-conventions.md) §6）：持续变化的内容不挂材质，无材质能力时外观不依赖材质。
8. **依赖后端动词的动作按后端契约划分**：要调用会话后端独有动词的（文件传输 / 剪贴板 / 遥测），归属由后端契约决定，动词就绪后再进共享单元。
9. **页面骨架统一**：页面按「页头 + 卡片」骨架组织，不另起一套页头 / 错误态 / 空态。
10. **交互按桌面光标（2in1/PC）适配，而非触屏**：目标设备以光标操作为主。触屏上"多点一下"无所谓，但光标下
    每弹一个新窗口都要求用户**移动并重新定位鼠标**——这是实打实的操作成本。因此**能用就地反馈就不新开弹窗**：
    异步加载态归触发控件本身、结果用 `showToast` 就地反馈；**只有需要用户从中做选择时**才弹列表 / 对话框，
    并尽量贴近触发点以减少鼠标行程。触屏友好的"分步弹窗流程"在光标下要按鼠标行程重新评估。

## 1. 设计 token

### 1.1 颜色（主题色，跟随系统深浅色）

来源 `resources/base|dark/element/color.json`，代码里一律 `$r('app.color.*')`。
**主题色在浅色模式下是浅色的**，因此任何前景色都必须与之配对使用，不可写死。

| token | 浅色 | 深色 | 用途 |
|---|---|---|---|
| `page_bg` | `#F2F3F5` | `#000000` | 页面底色 |
| `card_bg` | `#FFFFFF` | `#1B1B1D` | 卡片 / 浮层 / 圆钮底 |
| `input_bg` | `#F5F6F8` | `#26262A` | 输入框、次级按钮底、未选中 chip 底 |
| `divider` | `#ECECEC` | `#2C2C2E` | 分隔线 |
| `text_primary` | `#182431` | `#E8EAED` | 主文字、标题 |
| `text_secondary` | `#6B7785` | `#A2A9B4` | 次级文字、分节小标题 |
| `text_tertiary` | `#9AA4B0` | `#7C8592` | 三级文字、副标题、未选中项 |
| `accent` | `#0A59F7` | `#4A8BFF` | 主色：选中态、主按钮、强调动作 |
| `accent_hover` / `accent_press` | `#0743B8` / `#052C7A` | `#82B0FF` / `#B8D1FF` | 文本动作的悬停 / 按下态 |
| `accent_bg` | `#E6EEFF` | `#12233F` | 信息态徽标底（「运行中」） |
| `danger` | `#E84026` | `#F05A40` | 破坏性动作、已连接态圆钮 |
| `success` / `success_bg` | `#2E9E5B` / `#E6F6EC` | `#4CC77A` / `#17301F` | 「已连接」状态徽标 |
| `wake` | `#2E9E5B` | `#2FA05C` | 离线行「唤醒」钮底（深绿） |
| `hover_bg` | `#E7E9ED` | `#2E2E32` | 悬停底（图标按钮、chip） |
| `neutral` / `neutral_hover` | `#6E737B` / `#5A5F67` | `#8A8F98` / `#9298A2` | 实心中性动作钮（Moonlight 行的「应用」钮；与 accent / danger 并列为第三种动作色） |
| `star_on` | `#FFB300` | `#FFCA28` | 五角星选中填充（应用页「设为默认」的星标；未选中用 `text_primary`） |

约束：

- 需要**深色变体**的 SVG 图标放 `resources/dark/media/`（同名覆盖 base）。
- **除会话工具栏外不硬编码颜色**；`'#FFFFFF'`（选中 chip 文字、叠在 poster 上的徽标 / 图标）、`Color.Transparent`（未选中页签下划线）是允许的例外。
- **实心动作钮（`accent` / `danger` / `neutral` / `wake`，白图标）的深色版统一落在「白字对比 ≈3.3」这条带**：深底上不需要更高对比，各钮视觉轻重才一致。故 `wake` 深色取**同带深绿**（不照字体色那样大幅提亮），`neutral` 深色比浅色**更浅**（灰钮不反向做暗）。
- 选中态 chip 固定用「`accent` 底 + `#FFFFFF` 字」，不随主题切换为别的组合。

### 1.2 会话工具栏固定深色调色板（**不随主题**）

浮动在远端桌面画面上，背后颜色不可知，浅色 chrome 会随内容变得不可读，故整条工具栏固定深色。
常量定义在 `components/SessionChrome.ets`，**不要改成 `$r('app.color.*')`**。

| 常量 | 值 | 用途 |
|---|---|---|
| `TOOLBAR_BG` | `#CC1A1A1A` | 工具栏底（无材质时的兜底） |
| `TOOLBAR_FG` / `TOOLBAR_FG_SECONDARY` | `#FFFFFF` / `#B8BCC4` | 主 / 次文字与图标 |
| `TOOLBAR_CHIP_BG` | `#33FFFFFF` | 普通 chip 底 |
| `TOOLBAR_ACCENT_CHIP_BG` | `#4D3B82F6` | 剪贴板等强调 chip 底 |
| `TOOLBAR_NEUTRAL_CHIP_BG` | `#6E737B` | 非破坏性的会话退出 chip（Moonlight「断开」）：实心中性灰（同 `neutral`），与半透明白的普通 chip、`danger` 红都能分开 |
| `TOOLBAR_CHIP_BORDER` | `#59FFFFFF` | 描边（次级）chip 边框 |
| `TOOLBAR_PROGRESS` | `#5B9CF8` | 传输进度条 |
| `METRIC_GOOD` / `METRIC_WARN` / `METRIC_BAD` | `#4ADE80` / `#FBBF24` / `#F87171` | 遥测阈值着色 |

### 1.3 尺寸 / 圆角 / 字号 / 阴影 / 动画

| 类别 | 取值 |
|---|---|
| 卡片 | `maxWidth 720`、`padding` 编辑页 `28` / 设置页 `24`、圆角 `20` |
| 圆角 | 输入框 `10`、按钮 `12`、chip `8`、描边 chip `6`、列表行 `14`、图标 tile `11`、FAB 圆形 `28` |
| 字号 | 列表页标题 `20` Bold；页标题 / 状态卡标题 `17` Medium；行标题 `14`；分节小标题 `13`；副标题 `11`；chip `13`；遥测 `12`；徽标 `9–10` |
| 行高 | 带副标题的设置行 `58`；纯开关行 `48`；会话工具栏主行 `48`、遥测行 `24` |
| 按钮 | 编辑页高 `48` / 字号 `16`；设置页高 `44` / 字号 `15`；均 `radius 12` |
| chip | `padding { left:14, right:14, top:6, bottom:6 }`、`margin { right:8, bottom:8 }` |
| 阴影 | 卡片 `radius 24 / #14000000 / offsetY 8`；状态卡 `radius 30 / #33000000 / offsetY 10`；列表行 `radius 8 / #10000000 / offsetY 2`；FAB `radius 18 / #33000000 / offsetY 6` |
| 动画 | hover / press `120–150ms` `Curve.FastOutSlowIn`；控件出现 / 消失 `300 / 240ms`；工具栏滑入 `180ms` `Curve.EaseOut`；顶边提示线填充 `Curve.Linear`、淡出 `140ms` `Curve.FastOutSlowIn` |

约束：尺寸与字号**不在多个页面里各写一份**，收敛到常量或组件默认值（见「设计原则」与 §4）。
**`shadow()` 的 `radius` / `offsetX` / `offsetY` 单位是 px**（其余尺寸属性多为 vp），写数值前用 `UIContext.vp2px` 换算，
否则在高密度屏上会小到看不见。
**交互动效收敛到 `utils/UiMotion`**：控件各自持有 hover / press 状态（ArkUI 无共享的声明式 hover 效果），
但缩放目标、时长 / 曲线与出现 / 消失转场都从那里取（`hoverPressScale` / `popTransition` / `fadeTransition`），
避免同一控件在不同页面里手感不一致。出现 / 消失是**组件转场**（`if` 增删子树触发），转场自带动画参数，无需 `animateTo`。

## 2. 沉浸材质（系统材质）

材质三表面与配色方向（挂载机制、版本门槛、`ImmersiveModifier` 写法见 [`arkts-conventions.md`](arkts-conventions.md) §6）：

| 表面 | 挂载点 | 配色方向 |
|---|---|---|
| `CARD` | 列表 / 编辑 / 设置卡片 | 跟随系统深浅色；前景与兜底底色走 `$r()` |
| `OVERLAY` | Toast、Tips、右键菜单、AlertDialog | 跟随系统（系统自带前景色） |
| `FLOATING` | 会话工具栏、状态遮罩 | **固定深色，不跟随系统** |
| `CONTROL` | 图片上的小控件（应用 poster 的星标 / 动作钮） | 跟随系统；`THIN`，不加赋色 |

约束：

- 材质对象**建一次复用**（改参数触发系统重算材质，每帧新建会持续重绘）。
- 设了材质后背景要显式置 `Color.Transparent`，并与"不支持材质时回落兜底色"写在同一个三元表达式里。
- 材质**绝不挂在持续变化的内容**（远端画面）上；避免大面积、嵌套、与 `backgroundBlurStyle` 叠加。
- 无材质能力时所有表面退化为平色背景，**外观不得依赖材质存在**。

## 3. 布局模式

### 3.1 页面骨架

列表 / 编辑 / 设置三页共用同一骨架：

```
Column(page_bg, 100%×100%)
├─ 页头 HeaderBar（§3.2） + Divider
└─ 内容区（Scroll → 宽 720 居中卡片，§3.3；列表页为 List）
```

### 3.2 页头 HeaderBar

「左动作 + 中标题 + `Blank()` + 右侧 slot」，`.padding({ left:24, right:24, top:10, bottom:8 })`，下面一条 `Divider`。

- 左动作：`TextAction`（hover / press 有强调色反馈，`components/TextAction.ets`）。
- 标题：`17` Medium `text_primary`，`margin left 16`。
- 右侧 slot：编辑 / 设置页放协议页签（§5.1）；列表页用图标按钮。
- 列表页是变体：左侧 `app_icon`（44，圆角 11）+ `20` Bold 标题 + `12` 副标题，`padding top 20 / bottom 16`。

### 3.3 卡片与表单分节

- 卡片：`width 100%`、`constraintSize({ maxWidth:720 })`、圆角 20、`card_bg`（有材质时透明 + `CARD` 材质）、阴影、`padding`（编辑 28 / 设置 24）。
- 分节小标题：`13` `text_secondary`，`margin bottom 4`。
- 行：左列 `layoutWeight(1)`（标题 `14` `text_primary` + 副标题 `11` `text_tertiary`，`margin top 2`）+ 右侧控件；行间 `Divider`。
- 控件列（§4）右对齐；带副标题行高 `58`，纯开关行 `48`。
- **能力 / 前置条件的红字说明排在选项控件之上**：放在标题 / 副标题下方、开关 / chip / 输入框之上，用 `danger` `11`，
  **不要**放到控件下方或行尾（多选 chip 行同理——红字在 chip 行之上）。参照「超分辨率」「光标模式」。
  针对**当前所填值**的提醒（如自定义缩放的模糊警告，文案还引用了上方档位）可留在控件下方，不属此列。

### 3.4 折叠区（高级设置）

编辑页用折叠区承载「全局默认 + 单连接覆盖」。标题行左侧文案 + 右侧 `展开 / 收起`（`13` `accent`），整行 `.padding({ top:8, bottom:8 })`、`radius 8`、`HoverEffect.Highlight`。
展开后是若干「使用会话独立 XX 设置」行：左标题 + 右侧 `Toggle`，跟随全局时下方给一行 `11` `text_tertiary` 的「全局：…」摘要。

### 3.5 列表行

由 `ConnectionRow` 组件实现（§4.1）。`Row`（圆角 14、`card_bg` / CARD 材质、阴影、`padding { left:12, right:12, top:10, bottom:10 }`）：

- 引导 tile：协议图标 `40`、圆角 `10`；主标题 `15` + 状态徽标（`10`，「已连接」用 `success` / `success_bg`，「运行中」用 `accent` / `accent_bg`）、副标题 `12` `text_tertiary`。
- 右侧圆形动作钮：直径 `44`、圆角 `22`、`HoverEffect.Scale` + `bindTips`。连接钮随状态换图标 / 底色（`accent` ↔ `danger`，连接中显示 `LoadingProgress`）；Moonlight 行在连接钮**左侧**多一个应用钮。
- **主机不可达时（仅 Moonlight 行；未知 / 离线 / 服务未起三态）**：协议图标降不透明度（`0.45`）、隐藏应用钮，动作钮改为
  「刷新」（未知 / 服务未起，`ic_refresh`）/「唤醒」（离线，`ic_power` 画在 `wake`（深绿）上；**无 mac 或本机不在主机网段**时
  neutral 底 + `0.5` 不透明度且不可点）；
  **刷新钮**探测在途时换 `LoadingProgress`（含页面进入的自动兜底），**唤醒钮**不换加载态（其置灰与否随探测出的网段更新）；
  唤醒后的**开机检测期间**同样换 `LoadingProgress`（同「连接中」，再点取消检测）。服务未起的悬停提示为
  「主机已开机，请检查 Sunshine 是否运行」，不在主机网段的为「当前网络无法到达主机所在局域网，无法唤醒」。
  判据与四态见 [`moonlight-apps.md`](moonlight-apps.md) §2。
- 右键菜单（`Menu`）、拖拽排序（`List.onMove`）：`List` 本体保持全宽，左右内边距由 `ListItem` 的 `margin` 给，
  左 `64` / 右 `88`——右侧多留，为右下角 `FabButton`（`56` + 右 / 下 `24`）预留横向净空，卡片右缘不得压在 FAB 之下。
  菜单项分两组，各由一个 `MenuItemGroup` 承载：会话动作（连接 / 恢复 / 切换 / 断开 / 终止 / 刷新）在前，
  `编辑` / `删除` 在后，两组之间由分组分隔线隔开；分组归属由连接模型的 `RowMenuEntry.sessionAction` 给出。
  仅 `删除` 用 `danger`，其余项保持默认前景色。
- **菜单触发**：程序化 `bindContextMenu(isShown, …)`（`ResponseType` 二选一，右键 / 长按不能同吃）。鼠标由行的
  `onMouse`（右键按下）触发；触屏长按与排序拖拽共用同一次按压——`List.onMove` 的 `onLongPress` 只**武装**待开行、
  `onDragStart`（进入跟手拖动）**取消**，行 `onTouch` 抬起时才打开：**长按不拖动＝抬起弹菜单，长按拖动换序＝不弹**。
  该形式**不按被绑组件定位**，位置只能由 `ContextMenuOptions.offset` 给窗口坐标；`aboutToDisappear` 必须复位 `isShown`
  （否则残留 `true`，下次重渲染会重弹），`modalMode: ModalMode.TARGET_WINDOW` 让菜单外点击只关闭、不穿透到底下的行。

### 3.6 会话窗口

- 窗口模式：工具栏作为普通行布局在画面上方（渲染区自然扣除其高度）；全屏模式：工具栏浮在画面上，按指针分区自动隐藏 / 滑出（行为与判定见 [`session-layer.md`](session-layer.md) §9）。
- 画面由 `SessionPicture`（§4.1）承载：surface 按内容比例居中留边，两协议共用（RDP 取桌面 / 输出分辨率，Moonlight 取流分辨率）；窗口拖拽时由合成器按比例缩放，不依赖重绘。留边区域不接收指针，输入 / 焦点在 XComponent 上（系统滚动引擎的叠层只吃**两个轴向的轴事件**——触控板滚动与鼠标滚轮；触屏由策略层挡在外面，鼠标 / 键盘 / 焦点仍归 XComponent，见 [`session-layer.md`](session-layer.md) §8）。
- 自动隐藏的**显隐判定**由 `services/ToolbarAutoHide` 独占，两协议的会话页共用：页面提供实时状态（全屏 / 连接 / 钉住 / 延迟）并把结果（显示 / 揭示进度）镜像进自己的 `@State`，渲染与动效留在页面。
- 会话窗口的**运行时能力**归 `services/WindowController`：取本会话窗口、全屏检测、最小化 / 全屏切换、焦点 / 可见性监听。它们与自动隐藏判定一样**按需（opt-in）**——页面只调用 / 订阅自己反应的部分，抽取本身不申请任何用不到的能力。
- 状态遮罩：全屏居中、`#66000000` 底色上放状态卡；状态卡的图标 / 标题 / 副标题 / 按钮组合由连接状态驱动（§4）。
  **「正在连接」卡带显示阈值**（约 1000ms）：Moonlight 的会话窗在串流起来之前就开着，若卡立即出现只会一闪而过，
  故连接态的卡等这段阈值、仍未起来才显示；失败 / 断开 / 正在断开一律立即显示。RDP 窗口只在连上后才开，因此
  天然不出现连接卡。连接卡的副标题与 RDP 同形（连接目标 `targetLabel`），协议阶段文案不进卡片。
- 工具栏结构：主行（会话名 + 分辨率 + 传输区 + 剪贴板 chip + 全屏 / 最小化 / 断开）+ 可选遥测行（开发者模式）。
- **会话结束 chip 的着色**：RDP 只有「断开」→ `danger`（与主界面断开钮同色）；Moonlight 有「断开」与「关闭应用」
  两个结束动作，用第三种色把非破坏性的「断开」分出来——`TOOLBAR_NEUTRAL_CHIP_BG`（实心中性灰），更破坏性的
  「关闭应用」保持 `danger`。
- 主行左侧的「名称 + 分辨率」是同形的一段，由 `SessionTitle`（§4.1）渲染：名称 `14` 前景色，后面跟 `12`
  次级色的「宽 × 高」；协议差异只体现在分辨率后的**括号后缀**——RDP 是超分（`srIndicator`），Moonlight 是
  协商到的解码格式（`moonlightDecodeIndicator`，如 ` (H.264, CPU)`，软件解码才带 `, CPU`）。

## 4. 组件契约

`components/`（组件）与 `services/`、`utils/`（行为与工具）承载共享单元；共享的边界与原则见「设计原则（先读）」，本节只列清单与契约。

### 4.1 组件清单

| 组件 | 文件 | 参数（要点） | 用途 |
|---|---|---|---|
| `TextAction` | `TextAction.ets` | `label, muted?, onTap` | 悬停 / 按下变色的纯文本动作 |
| `ConnectionRow` | `ConnectionRow.ets` | `conn, state, hostTargetId, targetId, menuRequest, onEdit / onToggleConnection / onOpenApps / onTerminate / onRefresh / onDelete / onRequestMenu / onPressRelease` | 连接列表行（引导 tile + 名称/徽标 + 副标题 + 圆钮 + 右键菜单 + CARD 材质） |
| `SessionPicture` | `SessionPicture.ets` | `xComponentId, pictureController, contentWidth, contentHeight, revealBand, revealBandWidth, engineScroll, onPointerMouse/Touch/Hover/Axis, onSurfaceKey/Blur, onPictureArea, onRevealZone, onEngineScroll` | 会话画面（RDP / Moonlight 共用）：SURFACE 组件按内容比例居中留边，输入 / 焦点留在 XComponent 上（`engineScroll` 打开时挂 `Scroll2D`，它叠两个透明单轴 `Scroll`，只吃触控板与滚轮的两个轴向，触屏由策略层挡在外面，见 [`session-layer.md`](session-layer.md) §8）；全屏时另给顶部居中的 reveal 窄条（`revealBandWidth` = 占画面宽百分比，两端留空，透明命中） |
| `Scroll2D` | `Scroll2D.ets` | `viewWidth, viewHeight, onScroll(dx, dy)` | 会话滚动的 2D 引擎（RDP / Moonlight 共用）：两个透明单轴原生 `Scroll`（一竖一横）并行桥接、偏移钉住，逐帧交出 vp 增量；单轴件是拿到触控板惯性 / 打断所必需，见 [`session-layer.md`](session-layer.md) §8 |
| `StatusCard` | `StatusCard.ets` | `title, subtitle, primary?, secondary?, loading?, icon?, onPrimary, onSecondary` | 会话连接状态卡（RDP / Moonlight 共用） |
| `HeaderBar` | `HeaderBar.ets` | `title, leftLabel, onLeft, trailing`（`@BuilderParam`） | 页头（编辑 / 设置共用） |
| `ProtocolTabs` | `ProtocolTabs.ets` | `protocol, locked?, onChange` | RDP / Moonlight 下划线页签 |
| `MaterialCard` | `MaterialCard.ets` | `pad?, content`（`@BuilderParam`） | 宽 720 卡片 + `CARD` 材质 / 兜底色 + 阴影 |
| `FabButton` | `FabButton.ets` | `icon, tip?, onTap` | 右下角悬浮圆形主操作（新建 / 刷新；accent 底 + 阴影 + hover/press 反馈 + `OVERLAY` 提示） |
| `ControlDisc` | `ControlDisc.ets` | `icon, iconColor, diameter, glyph, gap?, tip?, loading?, animated?, onTap` | 叠在表面上的圆形图标控件（应用 poster 的星标 / 动作钮）：`CONTROL` 材质 + hover/press 缩放，`loading` 时图标换成转圈并置忙，`animated` 时另带出现/消失转场（动效走 `utils/UiMotion`） |
| `ChoiceChip` | `ChoiceChip.ets` | `label, selected, chipEnabled?, onTap` | 选中态 chip（缩放档位 / `srRatio` / `srChoice` / 分辨率模式等共用） |
| `SettingToggleRow` | `SettingToggleRow.ets` | `title, subtitle?, value, switchEnabled?, disabledReason?, onToggle` | 带开关的设置行 |
| `SettingSliderRow` | `SettingSliderRow.ets` | `title, subtitle?, value, min, max, step, unit?, precision?, switchEnabled?, disabledReason?, onChange` | 滑块行 |
| `ScaleSetting` | `ScaleSetting.ets` | `mode, allowNone?, allowCustomInput?, percentText, autoPercent, title?, subtitle?, noneHint?, warning?, customWarning?, onModeChange, onPercentTextChange` | 缩放设置块（RDP 会话桌面缩放 / Moonlight 基地版虚拟屏缩放共用）：`自动 / 手动（+可选「不额外设置」）` 模式行 + 档位 `SCALE_PRESETS` chip 组 + 可选自定义输入 + 红字提示（`不额外设置` 档隐藏能力类警告）；`@Prop` 值 + 回调把状态留在页面（`@Builder` 传值不刷新，故做成组件） |
| `ToolbarChip` | `SessionChrome.ets` | `label, chipBg?, fgColor?, icon?, hPad?, onTap` | 会话工具栏实心 chip |
| `SessionTitle` | `SessionChrome.ets` | `name, detail` | 工具栏左侧「名称 + 分辨率」标题（RDP / Moonlight 共用，差异只在 `detail` 后缀） |
| `OutlineChip` | `SessionChrome.ets` | `label, onTap` | 会话工具栏描边 chip（取消） |
| `MetricItem` | `SessionChrome.ets` | `text, metricWidth?, fgColor?, leftGap?, tip?` | 遥测字段：定宽排布；`metricWidth` 0 表示按内容宽（带宽项），`tip` 为空则不挂提示 |
| `TelemetryRow` | `SessionChrome.ets` | `fields: TelemetryField[]` | 开发者遥测行（RDP / Moonlight 共用）：页面交字段数组，行按 `TOOLBAR_TELEMETRY_HEIGHT` 渲染 |
| `TelemetryField` | `SessionChrome.ets` | `text, width, color, tip, gap` | 遥测行的一个字段（`width` 0 = 内容宽）；`metricColor` 给延迟阈值着色 |
| （调色板常量） | `SessionChrome.ets` | `TOOLBAR_*` / `METRIC_*` | 固定深色工具栏调色板与几何 |

约束：组件成员名**不得与通用属性同名**（`padding` / `enabled` 等会和 `CustomComponent` 基类冲突），故
`MaterialCard` 用 `pad`、`ChoiceChip` 用 `chipEnabled`、`SettingToggleRow` / `SettingSliderRow` 用 `switchEnabled`。

### 4.2 共享工具（`utils/`）

| 文件 | 内容 |
|---|---|
| `UiUtil.ets` | `showToast`（+`OVERLAY` 材质）、`lengthToNumber`（`Length` → vp 数值）、`showConfirm` / `showNotice`（统一的确认 / 提示弹窗，见下） |
| `PictureFit.ets` | 画面居中留边与映射：`fitPictureRect`（容器内按内容比例的居中矩形）、`mapPicturePoint`（画面局部坐标 → 远端像素，含夹取）；`SessionPicture` 与两个会话页共用 |
| `DisplayUtil.ets` | `defaultDisplay()`（容错查询默认显示，页面与服务共用） |
| `ScaleParse.ets` | 手动分辨率 / 缩放文本解析：`parseSizeValue` / `parseScaleValue` / `parsedTextValue` / `isScalePreset`，及 `MAX_DESKTOP_SIZE` |
| `MoonlightCodec.ets` | 编码器能力分类：`moonlightCodecSupport` / `moonlightCodecUsable` / `moonlightCodecSoftware` |
| `PairingPrompt.ets` | `promptPairingPin(ui, pin, pairing)`（配对 PIN 弹窗：四位数字居中放大；`pairing` 握手结束后自动关闭，返回值仅在用户取消时为 `false`） |
| `HostSearchPrompt.ets` | `showHostPicker(ui, hosts, title, addedAddresses)`（主机选择弹窗：只渲染**已搜到**的 mDNS 主机列表，`addedAddresses` 里的（已保存）主机置灰不可选，选中一行返回该主机，取消返回 `undefined`；搜索本身由页面跑，见下条约束） |
| `WolHelpPrompt.ets` | `promptWolHelp(ui)`（首次唤醒的 WoL 设置引导：自定义弹窗 + 可点教程链接，点「知道了」后 resolve；行为与一次性标记见 [`moonlight-apps.md`](moonlight-apps.md) §2） |
| `UiMotion.ets` | 交互动效 token 与工厂：`MOTION_HOVER_SCALE` / `MOTION_PRESS_SCALE` / `MOTION_HOVER_PRESS_MS` / `MOTION_CURVE`，`hoverPressScale(hovered, pressed)` 与转场工厂 `popTransition()` / `fadeTransition()` |

约束：**确认 / 提示弹窗一律走 `showConfirm` / `showNotice`，不在页面里直接写 `showAlertDialog`**——
两按钮弹窗恒为「左=取消（无副作用）+ 右=确认」，确认键颜色由 `danger` 选 `danger` / `accent`；
单按钮提示用 `showNotice`。材质（`OVERLAY`）与兜底都由工具负责，页面只给文案与回调。
带可点链接等自定义内容的引导（如 `WolHelpPrompt`）走 `ComponentContent` + `openCustomDialog`，不进这条。

约束：**异步操作的加载态归属触发控件，不单独弹一个"转圈"弹窗**（依据设计原则 §10：光标操作下弹窗要重新定位鼠标）——
就地把它自己的图标换成 `LoadingProgress`（图标本身即状态，参照连接钮 / 刷新 FAB；图标位较小时在原有图标上叠加），
期间禁用该控件的点击；耗时结果用 `showToast` 就地反馈（如"未发现主机，请确认…"）。**只有需要用户从中选择时才弹列表**，
且弹窗只承载结果、不承载加载态。

### 4.3 提示的类型与文案

提示只有两类：`showToast`（底部自动消失的短反馈）与弹窗（`showConfirm` 双按钮 / `showNotice` 单按钮）。
按**是否需要用户先决策**二选一，不按信息长短选：

- **弹窗**：动作**有副作用 / 不可逆**、需用户先确认 → `showConfirm`（删除连接、恢复默认、导入导出、
  取消文件传输、保存不含密码的连接）；关键失败需要用户停下知晓且**别处无持续呈现**（列表页发起连接失败）→
  `showNotice` 单按钮。
- **Toast**：其余就地反馈、不打断——成功 / 失败结果、并发阻拦、空数据（远端剪贴板为空）、发现结果（未发现主机）。
  会话内失败常驻 `StatusCard`，不再另弹 toast / 弹窗。

文案：**结论 + 具体对象**，一句话说清，不加句末标点。失败用「无法 X：原因」；
涉及多主机 / 多编码 / 多应用时**必须点名对象**（主机 IP、主机支持的编码、应用名），不用泛化措辞。

## 5. 协议差异的呈现约定

### 5.1 协议页签

`Text`（`14`，选中 Medium `text_primary` / 未选中 Normal `text_tertiary`）+ 下方 `Divider`（`strokeWidth 2`、`width 36`、选中 `accent` / 未选中 `Transparent`），`padding { left:12, right:12, top:4 }`。
编辑已有连接时对端页签置暗（`opacity 0.4`）且不可点；切换页签时端口随协议默认值切换。

### 5.2 列表

两种协议混排同一列表，靠引导 tile 图标区分；Moonlight 行的副标题显示默认应用名，RDP 显示账号。
Moonlight 行有两个圆钮（**应用在连接左侧**），RDP 行只有连接钮。

### 5.3 设置页分栏

顶部 RDP / Moonlight 两个协议页签；**通用项（触控板、键盘、会话、窗口）在两个页签都出现，但每个协议各存一份值**——UI 是同一段代码，靠切换数据源复用（详见 [`moonlight-connection.md`](moonlight-connection.md) §5）。

协议页签内的**分节骨架两协议一致**：**显示 → 串流（仅 Moonlight）→ 触控板 → 键盘 → 会话 → 连接特性 → 开发者（仅 RDP，开发者选项开启时）**。
「显示」承载画面与光标，「连接特性」承载协议相关开关（RDP：音频重定向 / 硬件加速 / 超分 / 忽略证书；Moonlight：音频重定向 / 低延迟音频）。
「通用」页签（窗口 / 配置备份 / 重置 / 开发者选项）只放应用级、与协议无关的项。
