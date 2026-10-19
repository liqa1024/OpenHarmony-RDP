# 界面框架

本文描述界面层的组织方式与契约：页面骨架、组件职责、状态归属、交互入口与协议差异的落点。
颜色、材质、字号、间距、圆角、阴影、动效曲线归 [`ui-visual.md`](ui-visual.md)；
提示通道选择、优先级与文案归 [`ui-feedback.md`](ui-feedback.md)；
会话窗口行为、输入内核、工具条自动隐藏与光标归 [`session-layer.md`](session-layer.md)；
应用结构、Ability 与页面职责表归 [`architecture.md`](architecture.md)；
设置字段、存储与生效时机归 [`settings-and-storage.md`](settings-and-storage.md)。

## 1. 分层与所有权

| 层 | 位置 | 责任 |
|---|---|---|
| 页面 | `pages/*.ets` | 组合根：读取 store、订阅状态、把数据与回调传给组件 |
| 领域组件 | `components/` 中带业务语义的组件 | 渲染连接、应用、会话外壳；不读 store |
| 基础组件 | 页头、卡片、分段、表单字段、状态视图、按钮与 chip | 只负责结构与交互状态，不感知协议与存储 |
| 视觉层 | `ui-visual.md` 与 `resources/` | 取值、材质、图标画法 |
| 服务层 | `services/`、`model/` | 会话、输入、存储、窗口、能力探测；不反向依赖页面 |

协议判断只允许出现在 `SavedConnection` 多态子类、`ConnectionLauncher` /
`SessionBackendFactory` 以及页面组合根。共享组件与会话外壳不出现
`if (protocol === …)` / `isMoonlight()`。

## 2. 页面骨架

### 2.1 Overview

用于 `Index` 与 `MoonlightAppsPage`。

- 结构：`Stack` 承载 `PageBackground`、页头、内容区与右下 `FabButton`。
- `PageHeader` 使用 brand 变体（应用图标、名称、副标题）或 standard 变体；
  trailing 槽位放页签、主机名或设置入口。
- 内容区自行选择 `List`（连接）或 `Scroll` + 可换行 `Flex`（应用磁贴）。
- 空、加载、错误统一用 `StateView`；错误态至少有标题、原因与重试/返回动作；主操作浮在右下，不与列表同层滚动。

### 2.2 Form

用于 `SettingsPage` 与 `EditConnectionPage`。

- 结构：`PageHeader` + `Scroll` + `Column` + `ContentCard`。
- 页头固定，内容整体滚动；`ContentCard` 提供内容列的最大宽度约束。
- `SettingsPage` 的协议页签放在 `PageHeader` trailing；区块统一由 `FormSection` 组织。
- `EditConnectionPage` 的基础字段由 `FormField` 组织；高级设置是可折叠区块。
- 设置页即时生效；编辑页由保存/连接动作提交，不自动落库。

### 2.3 Immersive

用于 `SessionPage` 与 `MoonlightSessionPage`。

- 结构：可选 `SessionToolbar` + `Stack`（`SessionPicture`、提示条、工具条、`StatusCard`）。
- 画面适配与输入承接由 `SessionPicture` 完成；页面把输入事件交给 `SessionShellController`。
- 工具条组合由 `SessionToolbar` 统一，协议差异通过入参表达。
- 覆盖层统一用 `StatusCard`，按窗口宽度自适应。
- 工具条显示、自动隐藏、触屏唤出与指针区判定见 [`session-layer.md`](session-layer.md)。

### 2.4 开发页

`GfxReplayPage` 使用独立按钮条、XComponent 网格与信息卡，不套用产品骨架，
也不纳入产品视觉契约。

### 2.5 骨架共享件

| 组件 | 责任 | 约束 |
|---|---|---|
| `PageHeader` | 标准/品牌页头 | 单个无参 `@BuilderParam`；调用处用尾随闭包且不挂通用属性 |
| `ContentCard` | 表单内容卡 | 单个内容槽；提供内容最大宽度约束 |
| `PageBackground` | 可选壁纸与遮罩层 | 不参与命中测试 |
| `FormSection` | 表单分段 | 标题、说明、可选动作、内容槽、底部分隔线 |
| `FormField` | 表单字段 | 标签、提示、错误、内容槽；错误优先于提示 |
| `StateView` | 页面级空/加载/错误态 | 占满父容器；按钮为空则不渲染 |
| `StatusCard` | 会话覆盖层状态卡 | 最大宽度约束，窄窗口不溢出 |

## 3. 页面状态与刷新

| 页面 | 状态来源 | 刷新触发 | 落库 |
|---|---|---|---|
| `Index` | 连接列表与状态 Map | 进入、`onPageShow`、会话/主机状态订阅 | 重排立即写顺序 |
| `EditConnectionPage` | 页面表单状态 | 进入时读取连接或全局默认 | 保存/删除/连接时提交 |
| `SettingsPage` | 页面表单状态 + `DebouncedSaver` | 进入、切换页签时同步 | 即时生效，防抖写盘 |
| `MoonlightAppsPage` | 应用列表、图标表、会话状态 | 进入、`onPageShow`、错误重试 | 默认应用与主机身份落库 |
| 会话页 | `SessionShellController` 驱动 | shell 的 host/hooks 回调 | 不落库 |

界面侧的约束：

- 设置页的写盘走 `utils/DebouncedSaver.ets`；页签切换、壁纸、导入/导出、重置与
  GFX 抓取开关走立即保存，离页前 flush。
- 滑块在 `SliderChangeMode.End` / `Click` 提交；拖动中的变化即时生效。
- 列表项会变化的状态用 `@Prop` 子组件承载；key 使用身份数据。
  `@Builder` 与 key 的详细规则见 [`arkts-conventions.md`](arkts-conventions.md)。

## 4. 组件契约

### 4.1 交互基础件

| 组件 | 责任 | 状态 |
|---|---|---|
| `TextAction` | 文本动作 | hover / press / focus；`muted` 控制层级 |
| `ControlDisc` | 圆形图标动作 | hover / press / focus；`frosted` 为覆盖在图片上的无底色厚磨砂，图标由调用方固定白色（收藏选中态用品牌色）；`loading` 时不可点、不可聚焦 |
| `FabButton` | 浮动主/次操作 | hover / press / focus；`primary` 区分主/次 tonal；`loading` 时不可点、不可聚焦 |
| `ChoiceChip` | 标准选择片 | `selected` / `chipEnabled`；禁用时不可聚焦 |
| `PromptDialog` | 提示/确认/通知外壳；通道语义见 [`ui-feedback.md`](ui-feedback.md) | 标题、说明、可选内容槽、取消/确认；所有自定义提示窗复用 |

可点击件必须可聚焦，Enter/Space 等同点击；禁用、加载、锁定态同时关闭可聚焦。
焦点框由系统绘制，视觉层可覆盖。

### 4.2 表单件

| 组件 | 责任 | 主要入参 |
|---|---|---|
| `SettingToggleRow` | 开关行 | 标题、副标题、值、启用状态、禁用原因、回调 |
| `SettingSliderRow` | 滑块行 | 标题、副标题、范围、步长、单位、精度、启用状态、`onChange`、`onCommit` |
| `ScaleSetting` | 缩放模式与预设 | 模式、是否允许“不额外设置”、是否允许自定义、当前值、提示与警告 |

能力不支持的设置项统一置灰并给出原因，不静默失败。

### 4.3 领域件

| 组件 | 责任 |
|---|---|
| `ConnectionRow` | 连接行渲染；状态与动作由 `ConnectionRowModel` 派生 |
| `ProtocolTabs` | 协议页签；编辑已有连接时可锁定 |
| `MoonlightAppTile` | 应用磁贴；运行态、默认态、加载态与动作圆钮 |
| `SessionToolbar` | 会话工具条组合；标题、固定、全屏/最小化/断开、可选剪贴板、可选传输状态、可选关闭应用 |
| `SessionPicture` | 画面适配、XComponent 承接、顶部触发带 |
| `Scroll2D` | 会话画面上的系统滚动惯性；不参与绘制 |
| `PadProbeOverlay` | 开发者触点探针；仅开发者模式 |

`ConnectionRowModel`（`model/ConnectionRowModel.ets`）统一派生图标、徽标、提示、
主操作、菜单是否可开与点击意图。连接行与菜单都读同一模型，避免显示与实际动作不一致。

`SessionToolbar` 只做组合与布局；工具条常量与触发区定义在
`services/ToolbarContract.ets`，服务层不从组件文件取这些常量。

## 5. 交互

- 焦点：行、圆钮、FAB、chip、页签、工具条按钮进入 Tab 顺序；`SessionPicture` 的
  XComponent 是可获焦叶节点并设置默认焦点。
- 连接行：右键与长按等价打开菜单；长按菜单与拖拽排序互斥，开始拖拽即取消菜单。
  正在断开时不打开菜单，动作不可点。
- 确认、通知、轻提示、内联反馈与加载态的适用面、优先级和文案统一见
  [`ui-feedback.md`](ui-feedback.md)；本节只保留具体交互行为。
- 会话工具条：窗口模式显示；全屏下按指针触发区或触屏下滑唤出；固定按钮与
  传输状态可保持常显。行为细节见 [`session-layer.md`](session-layer.md)。
- 剪贴板传输进度是共享工具条的可选能力：协议侧提供状态则显示，不提供则不占位。

## 6. 协议差异

界面层只表达协议在入口与呈现上的差异：

- 列表行图标、副标题、主操作与菜单项由 `SavedConnection` 多态给出。
- 编辑页与设置页的协议区块由页面组合根选择。
- 会话页由 `SessionBackend.pageRoute()` 选择；工具条差异通过 `SessionToolbar` 入参表达。
- 覆盖层：RDP 连接中立即显示；Moonlight 连接中延迟显示（进窗口后仍有握手）。
- 剪贴板：RDP 提供文件传输状态；Moonlight 按主机能力显示复制/粘贴。
- 光标、触屏唤出、自动隐藏与输入落地见 [`session-layer.md`](session-layer.md)；
  遥测口径与协议动作见 [`rdp-session.md`](rdp-session.md)、[`moonlight-session.md`](moonlight-session.md)。

共享组件不按协议分支；新增协议差异优先落在多态子类或页面组合根。

## 7. 命名与共享工具

- 页面 `XxxPage`，组件 PascalCase；单组件文件与导出的 struct 同名。
  工具条原语集中在 `SessionChrome.ets`，同领域的多个小组件可共用文件。
- 共享 UI 工具：
  - `utils/UiUtil.ets`：toast / confirm / notice；通道规则见 [`ui-feedback.md`](ui-feedback.md)。
  - `utils/UiMotion.ets`：hover/press 缩放与出现/消失转场。
  - `utils/DebouncedSaver.ets`：防抖落库。
  - `services/ToolbarContract.ets`：工具条高度、圆角、触发区与提示条常量。
  - `utils/SessionTags.ets`、`utils/TelemetryFormat.ets`、`utils/PictureFit.ets`、
    `utils/ScaleParse.ets`、`services/BackgroundImage.ets`：会话标签、遥测文案、
    画面适配、尺寸解析与壁纸导入。
