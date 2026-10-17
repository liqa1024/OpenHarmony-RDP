# ArkTS / ArkUI 约定

## 1. 严格 ArkTS（编译期硬约束）

- 禁用 `any` / `unknown`；
- 除受支持形式外禁用 `as` 类型断言；
- 禁用结构化类型、解构赋值、**未标注类型的对象字面量**（对象字面量必须有显式类型上下文）；
- `async` 必须显式标注 `Promise<T>`。

## 2. 状态与刷新（最常踩的运行时坑）

- **`@Builder` 的参数按值传递时不会随状态刷新**：需要响应状态变化的 builder 必须传**单个对象字面量**
  （按引用），并在 builder 内访问其属性。**这条在自定义组件内部同样成立**——组件因 `@Prop` 重渲染时，
  `build()` 里的 `if` 条件会重算（所以「按钮出现」正常），但经按值 `@Builder` 传下去的属性（如星标颜色）
  依旧不刷新。要动态刷新只有：传单个对象字面量（按引用）、改用「按回调传参」、或**包一层自定义组件**。
- **`@BuilderParam` 的初始化方式决定 `this` 指向（运行期崩溃）**：以 `this.xxx` **直接引用**把父组件 `@Builder`
  传给子组件 `@BuilderParam` 时，`this` 会变成**子组件实例**，builder 内 `this.someMethod(...)` 于是抛
  `TypeError: undefined is not callable`（编译期不报，点击/交互时才崩）。两种安全写法：**借用箭头函数**
  `Child({ content: () => { this.someBuilder(); } })`（箭头 `this` 绑定父组件）；或**尾随闭包**
  `Child({ ... }) { ... }`（内容按 `@Builder` 传给唯一的 `@BuilderParam`，`this` 绑定父组件）——尾随闭包要求
  子组件**有且仅有一个无参 `@BuilderParam`，且该组件不挂通用属性**。否则把内容做成独立子组件 / 显式参数。
- **`ForEach` 项里渲染会变的状态，正解是「子组件 + `@Prop`」，不是堆 key**：列表项写成**自定义组件**，
  把会变的状态用 `@Prop` 传入——父组件重渲染时 `@Prop` 会**在原地更新**（`ConnectionRow` 的 `state`
  就是这样驱动连接钮的加载态）。**不要**用页面 `@Builder` 内联整个列表项：`@Builder` 按值传参不刷新，
  项会被冻住（点星标、连上会话、图标到位全都不动）。
- key **只需随「身份数据」变化**：本项目统一用 `${conn.id}#${conn.updatedAt}`（`updatedAt` 每次保存都刷新）。
  应用管理页的 tile（`components/MoonlightAppTile`）因 **图标位图 `PixelMap` 无法走 `@Prop`**（`@Prop`
  深拷贝会丢 NAPI 复杂类型的类型），图标走**普通字段**、key 用 `${app.id}#${该 tile 自己的图标是否已就绪}`
  （按该 tile 判断，不用图标表 `size`：size 每到位一张图就变，会让**所有** tile 一起重建、并重放控件出现转场），
  只在图标到位时重建该 tile；默认 / 正在运行 / 会话态一律走 `@Prop`。
- **`JSON.parse` 反序列化**：`as` 只影响编译期，**不会**转换成员类型，也**不会**恢复类默认值。
  统一 `JSON.parse(text) as Record<string, Object>` 再逐字段读取（见 `ConfigTransfer.readSettings`）；
  不要直接 `as` 成业务类后依赖其字段类型或默认值。

## 3. 路由

- 路由参数必须是**具名接口**（如 `EditConnectionParams`），通过 `this.getUIContext().getRouter()` 传递。
- 页面间不传密码等敏感值：会话窗口只传**会话 key**（见 [`session-layer.md`](session-layer.md) §5）。

## 4. 主题与资源

- 主题色在 `resources/base|dark/element/color.json`，代码里用 `$r('app.color.*')`；深浅色跟随系统。
- 需要**深色变体**的 SVG 图标放 `resources/dark/media/`（同名文件覆盖 base）。
- 文案/尺寸尽量走资源与常量，不要在多个页面里各写一份。
- 各 token 的**用途**、尺寸 / 圆角 / 字号取值与组件规范见 [`ui-design.md`](ui-design.md)；本节只讲资源与主题机制。

## 5. 与原生交互

- 每个会话窗口一个会话后端 `SessionBackend`（RDP 实现 `RdpSessionBackend` 内部独占一个 `RdpNative` 实例与
  原生 handle）；原生事件带 handle，按 handle 路由；`SessionManager` 按 `sessionKey` 经 `SessionBackendRegistry`
  关联窗口与后端。两个会话页共用 `services/SessionShellController`（协议无关外壳）与
  `services/SessionViewDelegate`（协议钩子）。
- **两协议的通用 / 抽象层不得出现协议判断**：连接行为放 `SavedConnection` 多态子类（`RdpConnection` /
  `MoonlightConnection`），连接发起分发放 `services/ConnectionLauncher`，会话外壳差异经 `SessionViewDelegate`
  注入。`if (protocol === …)` / `isMoonlight()` / 协议布尔标志只允许出现在协议专属类与唯一组合入口（工厂）。
- 原生侧能力差异（音频、GPU 加速等）走 `services/DeviceCapabilities.ets` 的 `Capability{supported, reason}`：
  UI 置灰 + 说明原因，不要静默失败（见 [`architecture.md`](architecture.md) §5）。
- 设置项、连接字段、原生开关三者的对应关系见 [`settings-and-storage.md`](settings-and-storage.md)。

## 6. 内容控件磨砂与弹窗系统材质

ArkUI 的系统材质 `uiMaterial` 自 **API 26.0.0** 起提供。材质等级由**设备算力**决定（只可读：
`getGlobalMaterialLevel()`），效果强弱由**系统"沉浸光感"设置**决定：两者都由系统自适应、**应用无从设置**。
本工程只用它做**弹窗背板**；大块内容表面铺**半透明主题底**（`surface_bg`），只有小面积控件与会话工具栏用
**组件磨砂**（`backgroundBlurStyle`），两者不要混在同一个表面上。统一入口是 `services/SystemMaterial.ets`。

- **生效范围（targetSdkVersion ≥ 26.0.0）**：弹窗类组件与弹窗类接口（Dialog、Toast、Tips、右键菜单、
  半模态等）以及 `Slider` / `Toggle` / `Select` 在页面**任意区域**生效；其他组件**只在
  `Navigation` / `NavDestination` 标题栏，或横向 `Tabs`（`barPosition: BarPosition.End`）的底部 TabBar 内**
  生效。范围外设置 `systemMaterial` **不渲染材质，但仍会把 `backgroundColor` 清成透明、
  把 `borderWidth` 清空**——表面直接变成全透明、没有边界，而不是回退到磨砂。
  因此**内容表面（卡片 / 会话工具栏 / 小控件）不挂系统材质**：大块内容表面（页头、编辑 / 设置卡片、
  连接列表行）铺 `surface_bg` 半透明底，小控件与会话工具栏用组件磨砂；系统材质只出现在弹窗 options 里。
  范围由放置位置决定，调用点无从判断。
- **弹窗材质的构造**：
  1. `deviceInfo.sdkApiVersion >= 26`：老系统上 `deviceInfo.apiAvailable()` **自身不存在**，
     只能先用底座版本号兜住；
  2. `if (deviceInfo.apiAvailable('26.0.0'))`：版本号必须是**字面量、直接写在 if 里**
     （不支持封装、变量、三元或 `&&`/`||`），编译器据此消除 `compatibleSdkVersion` 的兼容性告警；
  3. `uiMaterial.isImmersiveMaterialSupported()`：只用于弹窗——为 `true` 才构造材质并缓存；
     为 `false` 时 `SystemMaterial.of` 返回 `undefined`，自定义弹窗的内容自己铺 `card_bg`。
  该判定在 UI 就绪前是占位 `false`，所以它只服务于运行时弹窗的兜底，
  **不用于内容表面**：内容控件 / 工具栏的磨砂样式与版本/能力无关，没有判定、没有重试、没有外观分支。
- **弹窗材质走 options 对象**（Toast、Tips、右键菜单、AlertDialog/`showAlertDialog`、`openCustomDialog`）：
  作为字段传入，老系统读到它只会忽略，无需额外包装。**这条路径不要再自己设 `backgroundBlurStyle`**：
  弹窗类组件自带系统默认背景（DEFAULT 模式下在未设背景 / 模糊 / 阴影时还会默认带材质），
  主动设模糊反而会把系统默认材质关掉，且不会更通透。
- **自定义弹窗的内容不要再铺不透明底**：`openCustomDialog` 的内容根节点若铺 `card_bg`，会盖住弹窗背板材质。
  材质生效时内容根节点透明、由背板出玻璃；只有在没有材质的旧系统上才由内容自己铺 `card_bg` 兜底
  （`SystemMaterial.of(DIALOG) === undefined` 时）。这是**唯一**保留版本差异的地方。
- **材质对象建一次复用**：改参数会触发系统重算材质，每次渲染新建会让材质层一直重绘。
- **磨砂是确定样式，不留材质/兜底双分支**：`FrostedSurface` → `FrostedModifier`，
  `CARD`→`COMPONENT_REGULAR`、`CONTROL` / `FLOATING`→`COMPONENT_THIN`，`FLOATING` 固定
  `colorMode: ThemeColorMode.DARK`。磨砂填充自带跟随深浅色的底色；需要固定色调时**可以直接铺
  `backgroundColor`**，磨砂叠在它之上（`FLOATING` 的 `TOOLBAR_BG`、`CONTROL` 的 `control_bg` 就是这样）。
  旧材质时代"不透明底会盖住材质"的限制不适用于组件磨砂。不探测能力、不因系统版本改变分支。
  - 小按钮的 `control_bg` 与表面**反相**：浅色比表面略深、深色比表面略浅；前景用 `control_accent` /
    `control_danger` / `star_on`，深色主题取更浅的变体。
  - 大块内容表面（页头、编辑 / 设置卡片、连接列表行）铺 `surface_bg` 半透明底、配圆角与轻阴影，
    不挂磨砂：壁纸透出，同时保住文字对比。
  - `FLOATING` 的投影由调用点加（`TOOLBAR_SHADOW`），不进 modifier：它浮在远端画面上，需要与画面分层。
- **表面清单**（不在表中的大块内容表面一律铺 `surface_bg`，不挂磨砂）：
  | 表面 | 挂载点 | 形态 |
  |---|---|---|
  | `CARD` | 会话连接状态卡（压在会话遮罩上的面板） | 组件磨砂 `COMPONENT_REGULAR`（跟随系统） |
  | `CONTROL` | 小按钮与图片上的小控件（页头动作、应用 poster 的星标 / 动作钮） | `control_bg` 底 + 组件磨砂 `COMPONENT_THIN`（跟随系统） |
  | `FLOATING` | 会话工具栏 | 半透明深色底（`TOOLBAR_BG`）+ 组件磨砂 `COMPONENT_THIN` + `colorMode: ThemeColorMode.DARK`（固定深色） |
  | `OVERLAY` | Toast、Tips、右键菜单 | options `systemMaterial` `THICK` |
  | `DIALOG` | 弹窗（`AlertDialog`、`showConfirm` / `showNotice`、`openCustomDialog`） | options `systemMaterial` `ULTRA_THICK` |
  - `OVERLAY` 与 `DIALOG` **必须分开**：系统给弹窗的默认档是 `ULTRA_THICK`、给 Toast / 菜单的是 `THICK`，
    而**显式传 `systemMaterial` 会覆盖系统默认**，合成一档就是把弹窗做得比系统默认更透。
  - `FLOATING` 固定深色、不跟随系统：它是压在远端画面上的 chrome，前景色用
    `components/SessionChrome.ets` 的固定深色常量（`TOOLBAR_*` / `METRIC_*`），**不要**改成
    `$r('app.color.*')`——那正是"浅底白字"的来源。
  - 磨砂控件的前景按主题 token 选色，浅色 / 深色分别校验：按钮**文字 ≥ 4.5:1、图标 ≥ 3:1**；
    二级文字用 `text_secondary`，`text_tertiary` 在浅色磨砂上不足该门槛，不用于磨砂按钮。
  - **材质效果不以模拟器为准**：模拟器可能渲染不出系统材质（Dialog / Tips 尤其）。
    不要据此调样式/取值，材质相关改动只在真机上判定。
- **摆放约束**（直接决定性能与功耗，改挂载点前先看）：
  - **磨砂只铺在小面积控件上**——它每帧重新采样背景；大块内容表面用 `surface_bg` 半透明底，
    滚动列表逐项挂磨砂、给整页大容器挂磨砂都是已知的坏味道；
  - 避免大面积、避免嵌套层叠；小按钮的磨砂与阴影只属于控件自身，**不要再叠材质阴影**。
- **配套**：
  - 磨砂控件**不加描边**：磨砂自带形状与填充；需要固定色调时可以直接铺底色（`FLOATING` 铺
    `TOOLBAR_BG`），底色与磨砂不互斥。描边只是为了"补材质的边界"，与磨砂的设计语言冲突。
  - 大块内容表面铺 `surface_bg` 并保留自己的轻阴影；不加描边，体积感与分区交给底色、圆角和阴影。
  - 弹窗材质不赋色、不启用 `colorInvert`：只取系统默认档位，前景由系统或主题色负责。
