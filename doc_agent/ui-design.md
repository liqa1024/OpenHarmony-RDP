# UI 设计规范（视觉语言 / 布局模式 / 组件契约）

本文件是 HmRdp 界面的**设计语言总纲**：设计原则、设计 token、布局模式、组件契约与共享工具。
它只回答"界面长什么样、由哪些组件组合、参数怎么给"，**不描述行为与数据语义**：

- 会话窗口 / 输入 / 工具栏行为与遥测口径 → [`session-and-input.md`](session-and-input.md)；
- 设置项 / 连接存储 / 生效时机 → [`settings-and-storage.md`](settings-and-storage.md)；
- Moonlight 协议差异（字段、分栏、图标语义） → [`moonlight-connection.md`](moonlight-connection.md)；
- ArkTS / ArkUI 语法、状态刷新、主题资源规则、沉浸材质 API 与摆放机制 → [`arkts-conventions.md`](arkts-conventions.md)。

## 设计原则（先读）

规范思路先于下面的具体取值；凡"加共享单元 / 抽组件 / 新增控件"都按这几条办：

1. **共享按逻辑分类，不按当下使用**：抽取边界是**逻辑类别**（同一类卡片 / 页头 / chip / 窗口能力 / 自动隐藏……），不是"当前哪一侧在用"。属同一逻辑类的能力**一并抽出**，即使暂时只有一侧使用——这正是保留扩展性的方式（另一侧要用时直接取用，而不是再抄一份）。有协议独有但同类的能力同样抽出，由页面选择用不用。
2. **抽取必须是"可选能力"**：共享单元按需（opt-in）使用——不主动订阅事件、不申请能力，避免"为了通用"让窗口 / 页面背上用不到的行为。**范围可以大，但不能有未被调用的副作用。**
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
| `danger` | `#E84026` | `#F05A40` | 破坏性动作、已连接态圆钮 |
| `success` / `success_bg` | `#2E9E5B` / `#E6F6EC` | `#4CC77A` / `#17301F` | 状态徽标（如「已连接」「运行中」） |
| `hover_bg` | `#E7E9ED` | `#2E2E32` | 悬停底（图标按钮、chip） |

约束：

- 需要**深色变体**的 SVG 图标放 `resources/dark/media/`（同名覆盖 base）。
- **除会话工具栏外不硬编码颜色**；`'#FFFFFF'`（选中 chip 文字、叠在 poster 上的徽标 / 图标）、`Color.Transparent`（未选中页签下划线）是允许的例外。
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
| 动画 | hover / press `120–150ms` `Curve.FastOutSlowIn`；工具栏滑入 `180ms` `Curve.EaseOut`；顶边提示线填充 `Curve.Linear`、淡出 `140ms` `Curve.FastOutSlowIn` |

约束：尺寸与字号**不在多个页面里各写一份**，收敛到常量或组件默认值（见「设计原则」与 §4）。
**`shadow()` 的 `radius` / `offsetX` / `offsetY` 单位是 px**（其余尺寸属性多为 vp），写数值前用 `UIContext.vp2px` 换算，
否则在高密度屏上会小到看不见。

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

### 3.4 折叠区（高级设置）

编辑页用折叠区承载「全局默认 + 单连接覆盖」。标题行左侧文案 + 右侧 `展开 / 收起`（`13` `accent`），整行 `.padding({ top:8, bottom:8 })`、`radius 8`、`HoverEffect.Highlight`。
展开后是若干「使用会话独立 XX 设置」行：左标题 + 右侧 `Toggle`，跟随全局时下方给一行 `11` `text_tertiary` 的「全局：…」摘要。

### 3.5 列表行

由 `ConnectionRow` 组件实现（§4.1）。`Row`（圆角 14、`card_bg` / CARD 材质、阴影、`padding { left:12, right:12, top:10, bottom:10 }`）：

- 引导 tile：协议图标 `40`、圆角 `10`；主标题 `15` + 状态徽标（`10`，`success` / `success_bg`）、副标题 `12` `text_tertiary`。
- 右侧圆形动作钮：直径 `44`、圆角 `22`、`HoverEffect.Scale` + `bindTips`。连接钮随状态换图标 / 底色（`accent` ↔ `danger`，连接中显示 `LoadingProgress`）；Moonlight 行在连接钮**左侧**多一个应用钮。
- 右键菜单（`Menu`）、拖拽排序（`List.onMove`）：`List` 本体保持全宽，左右内边距由 `ListItem` 的 `margin`（`left/right 52`）给。

### 3.6 会话窗口

- 窗口模式：工具栏作为普通行布局在画面上方（渲染区自然扣除其高度）；全屏模式：工具栏浮在画面上，按指针分区自动隐藏 / 滑出（行为与判定见 [`session-and-input.md`](session-and-input.md) §3）。
- 自动隐藏的**显隐判定**由 `services/ToolbarAutoHide` 独占（指针分区 / 揭示停留 / 收起延迟 / 传输钉住），两协议的会话页共用：页面提供实时状态（全屏 / 连接 / 钉住 / 延迟）并把结果（显示 / 揭示进度）镜像进自己的 `@State`，渲染与动效留在页面。
- 会话窗口的**运行时能力**归 `services/WindowController`：取本会话窗口、全屏检测、最小化 / 全屏切换、焦点 / 可见性监听。它们与自动隐藏判定一样**按需（opt-in）**——页面只调用 / 订阅自己反应的部分，抽取本身不申请任何用不到的能力。
- 状态遮罩：全屏居中、`#66000000` 底色上放状态卡；状态卡的图标 / 标题 / 副标题 / 按钮组合由连接状态驱动（§4）。
  **「正在连接」卡带显示阈值**（约 1000ms）：Moonlight 的会话窗在串流起来之前就开着，若卡立即出现只会一闪而过，
  故连接态的卡等这段阈值、仍未起来才显示；失败 / 断开 / 正在断开一律立即显示。RDP 窗口只在连上后才开，因此
  天然不出现连接卡。连接卡的副标题与 RDP 同形（连接目标 `targetLabel`），协议阶段文案不进卡片。
- 工具栏结构：主行（会话名 + 分辨率 + 传输区 + 剪贴板 chip + 全屏 / 最小化 / 断开）+ 可选遥测行（开发者模式）。
- 主行左侧的「名称 + 分辨率」是同形的一段，由 `SessionTitle`（§4.1）渲染：名称 `14` 前景色，后面跟 `12`
  次级色的「宽 × 高」；协议差异只体现在分辨率后的**括号后缀**——RDP 是超分（`srIndicator`），Moonlight 是
  协商到的解码格式（`moonlightDecodeIndicator`，如 ` (H.264, CPU)`，软件解码才带 `, CPU`）。

## 4. 组件契约

`components/`（组件）与 `services/`、`utils/`（行为与工具）承载共享单元；共享的边界与原则见「设计原则（先读）」，本节只列清单与契约。

### 4.1 组件清单

| 组件 | 文件 | 参数（要点） | 用途 |
|---|---|---|---|
| `TextAction` | `TextAction.ets` | `label, muted?, onTap` | 悬停 / 按下变色的纯文本动作 |
| `ConnectionRow` | `ConnectionRow.ets` | `conn, state, appEnabled?, onEdit, onToggleConnection, onOpenApps, onDelete` | 连接列表行（引导 tile + 名称/徽标 + 副标题 + 圆钮 + 右键菜单 + CARD 材质） |
| `StatusCard` | `StatusCard.ets` | `title, subtitle, primary?, secondary?, loading?, icon?, onPrimary, onSecondary` | 会话连接状态卡（RDP / Moonlight 共用） |
| `HeaderBar` | `HeaderBar.ets` | `title, leftLabel, onLeft, trailing`（`@BuilderParam`） | 页头（编辑 / 设置共用） |
| `ProtocolTabs` | `ProtocolTabs.ets` | `protocol, locked?, onChange` | RDP / Moonlight 下划线页签 |
| `MaterialCard` | `MaterialCard.ets` | `pad?, content`（`@BuilderParam`） | 宽 720 卡片 + `CARD` 材质 / 兜底色 + 阴影 |
| `FabButton` | `FabButton.ets` | `icon, tip?, onTap` | 右下角悬浮圆形主操作（新建 / 刷新；accent 底 + 阴影 + hover/press 反馈 + `OVERLAY` 提示） |
| `ChoiceChip` | `ChoiceChip.ets` | `label, selected, chipEnabled?, onTap` | 选中态 chip（`scale / srRatio / srChoice / ml*` 共用） |
| `SettingToggleRow` | `SettingToggleRow.ets` | `title, subtitle?, value, switchEnabled?, disabledReason?, onToggle` | 带开关的设置行 |
| `SettingSliderRow` | `SettingSliderRow.ets` | `title, subtitle?, value, min, max, step, unit?, precision?, switchEnabled?, disabledReason?, onChange` | 滑块行 |
| `ToolbarChip` | `SessionChrome.ets` | `label, chipBg?, fgColor?, icon?, hPad?, onTap` | 会话工具栏实心 chip |
| `SessionTitle` | `SessionChrome.ets` | `name, detail` | 工具栏左侧「名称 + 分辨率」标题（RDP / Moonlight 共用，差异只在 `detail` 后缀） |
| `OutlineChip` | `SessionChrome.ets` | `label, onTap` | 会话工具栏描边 chip（取消） |
| `MetricItem` | `SessionChrome.ets` | `text, metricWidth?, fgColor?, leftGap?, tip?` | 遥测字段：定宽排布；`metricWidth` 0 表示按内容宽（带宽项），`tip` 为空则不挂提示 |
| （调色板常量） | `SessionChrome.ets` | `TOOLBAR_*` / `METRIC_*` | 固定深色工具栏调色板与几何 |

约束：组件成员名**不得与通用属性同名**（`padding` / `enabled` 等会和 `CustomComponent` 基类冲突），故
`MaterialCard` 用 `pad`、`ChoiceChip` 用 `chipEnabled`、`SettingToggleRow` / `SettingSliderRow` 用 `switchEnabled`。

### 4.2 共享工具（`utils/`）

| 文件 | 内容 |
|---|---|
| `UiUtil.ets` | `showToast`（+`OVERLAY` 材质）、`lengthToNumber`（`Length` → vp 数值）、`showConfirm` / `showNotice`（统一的确认 / 提示弹窗，见下） |
| `DisplayUtil.ets` | `defaultDisplay()`（容错查询默认显示，页面与服务共用） |
| `ScaleParse.ets` | 手动分辨率 / 缩放文本解析：`parseSizeValue` / `parseScaleValue` / `parsedTextValue` / `isScalePreset`，及 `MAX_DESKTOP_SIZE` |
| `MoonlightCodec.ets` | 编码器能力分类：`moonlightCodecSupport` / `moonlightCodecUsable` / `moonlightCodecSoftware` |
| `PairingPrompt.ets` | `promptPairingPin(ui, pin, pairing)`（配对 PIN 弹窗：四位数字居中放大；`pairing` 握手结束后自动关闭，返回值仅在用户取消时为 `false`） |
| `HostSearchPrompt.ets` | `showHostPicker(ui, hosts, title, addedAddresses)`（主机选择弹窗：只渲染**已搜到**的 mDNS 主机列表，`addedAddresses` 里的（已保存）主机置灰不可选，选中一行返回该主机，取消返回 `undefined`；搜索本身由页面跑，见下条约束） |

约束：**确认 / 提示弹窗一律走 `showConfirm` / `showNotice`，不在页面里直接写 `showAlertDialog`**——
两按钮弹窗恒为「左=取消（无副作用）+ 右=确认」，确认键颜色由 `danger` 选 `danger` / `accent`；
单按钮提示用 `showNotice`。材质（`OVERLAY`）与兜底都由工具负责，页面只给文案与回调。

约束：**异步操作的加载态归属触发控件，不单独弹一个"转圈"弹窗**（依据设计原则 §10：光标操作下弹窗要重新定位鼠标）——
就地把它自己的图标换成 `LoadingProgress`（图标本身即状态，参照连接钮 / 刷新 FAB；图标位较小时在原有图标上叠加），
期间禁用该控件的点击；耗时结果用 `showToast` 就地反馈（如"未发现主机，请确认…"）。**只有需要用户从中选择时才弹列表**，
且弹窗只承载结果、不承载加载态。

## 5. 协议差异的呈现约定

### 5.1 协议页签

`Text`（`14`，选中 Medium `text_primary` / 未选中 Normal `text_tertiary`）+ 下方 `Divider`（`strokeWidth 2`、`width 36`、选中 `accent` / 未选中 `Transparent`），`padding { left:12, right:12, top:4 }`。
编辑已有连接时对端页签置暗（`opacity 0.4`）且不可点；切换页签时端口随协议默认值切换。

### 5.2 列表

两种协议混排同一列表，靠引导 tile 图标区分；Moonlight 行的副标题显示默认应用名，RDP 显示账号。
Moonlight 行有两个圆钮（**应用在连接左侧**），RDP 行只有连接钮。

### 5.3 设置页分栏

顶部 RDP / Moonlight 两个协议页签；**通用项（触控板、键盘、会话、窗口）在两个页签都出现，但每个协议各存一份值**——UI 是同一段代码，靠切换数据源复用。
