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

## 6. 沉浸光感（系统材质）

ArkUI 的系统材质 `uiMaterial` 自 **API 26.0.0** 起提供。材质等级由**设备算力**决定（只可读：
`getGlobalMaterialLevel()`），效果强弱由**系统"沉浸光感"设置**决定：两者都由系统自适应、**应用无从设置**。
应用只决定"哪些表面挂材质、挂哪一档"（档位≠强度）。统一入口是 `services/SystemMaterial.ets`。

- **门槛判断**（三者都过才返回材质，否则返回 `undefined`，该表面改由同档组件磨砂兜底）：
  1. `deviceInfo.sdkApiVersion >= 26`：老系统上 `deviceInfo.apiAvailable()` **自身不存在**，
     只能先用底座版本号兜住；
  2. `if (deviceInfo.apiAvailable('26.0.0'))`：版本号必须是**字面量、直接写在 if 里**
     （不支持封装、变量、三元或 `&&`/`||`），编译器据此消除 `compatibleSdkVersion` 的兼容性告警；
  3. `uiMaterial.isImmersiveMaterialSupported()`。
- **能力判定要「首用时求值 + 只缓存真答案」**：
  `uiMaterial` 的判定在**应用 UI 起来之前**返回**未初始化的占位答案**——`isImmersiveMaterialSupported()`
  为 `false`、`getGlobalMaterialLevel()` 为 `SMOOTH`（低算力档）；首个页面起来后才变成 `true` + `EXQUISITE`，
  两者相差**百毫秒级**。由此有两条：
  - 判定**不能**在模块求值期定死：`const SUPPORTED = probe()` 一旦落在 UI 就绪之前，`false` 就被永久钉住，
    症状是**全部表面都没有材质**——所有调用点恒走兜底磨砂（真机上与材质的通透感、高光差别明显），
    Toast / Tips / 右键菜单这类 options 路径同样不生效（它们与 modifier 路径共用这一个判定）。
  - 判定模块**可以出现在启动期 import 链上**：失效的根源只是"在 UI 就绪前求值"，与谁引用它无关——
    `utils/UiUtil`（Toast / 弹窗工具）被 `EntryAbility` 直接引到，判定照常。
  做法（`services/SystemMaterial.ets`）：首次使用时求值，**只缓存真答案**，假答案按固定间隔重问（节流）。
  判定、**底色**与"挂什么"一起收敛在 `ImmersiveModifier` 内部，调用点只声明表面，于是真答案到来的那一帧
  底色与材质同时就位，不会出现"底色已透明、材质还没挂"的中间态。**判定只作用于此后渲染到的节点**：
  apply 仅在初始化与组件自身状态变化时触发，早先按假答案挂过兜底磨砂、其后不再重渲染的表面会停在兜底。
- **两种挂法**，取决于组件如何暴露该属性：
  - **通用属性 `systemMaterial`** → 用 `ImmersiveModifier`（`AttributeModifier`）施加。
    该属性 API 26 才存在，直接内联写在组件上会让老系统的整棵组件构建失败（不是降级）；
    组件类型不同，`AttributeModifier<T>` 不通用，按组件属性类型实例化。
  - **options 对象**（Toast、Tips、右键菜单、AlertDialog/`showAlertDialog`）→ 作为字段传入，
    老系统读到它只会忽略，无需额外包装。**这条路径不要再自己设 `backgroundBlurStyle`**：
    弹窗类组件自带系统默认背景（DEFAULT 模式下在未设背景 / 模糊 / 阴影时还会默认带材质），
    主动设模糊反而会把系统默认材质关掉，且不会更通透。
- **材质对象建一次复用**：改参数会触发系统重算材质，每次渲染新建会让材质层一直重绘。
- **无材质时的兜底是「同档组件磨砂」，不是平色**：兜底档位与材质样式同档
  （`CARD`→`COMPONENT_REGULAR`、`CONTROL` / `FLOATING`→`COMPONENT_THIN`，`FLOATING` 另加深色档；
  `OVERLAY` / `DIALOG` 走 options 路径、不经过 modifier，无材质时交给系统默认背景），
  与材质一样由 `ImmersiveModifier` 在**互斥分支**里挂。互斥是硬约束：`backgroundBlurStyle` 的视觉层级高于材质层，
  与材质同时写在组件上会把材质整片盖掉（连设 `NONE` 也会建这一层，一样盖住），所以是"有材质就完全不设模糊"，
  而不是"设成 NONE"。组件磨砂自带跟随深浅色的底色，所以无材质时跟随系统的表面不需要自己的底色。
- **表面清单、材质档位与配色方向**（`SystemMaterial.MaterialSurface`）——档位即"这块表面要遮住多少背后内容"，
  每个表面都必须**自己指名**，不要靠默认值兜（漏配会静默继承另一个表面的档位）：
  | 表面 | 挂载点 | 档位 | 赋色 / 阴影 | 配色方向 |
  |---|---|---|---|---|
  | `CARD` | 列表 / 编辑 / 设置卡片、连接状态卡片 | `REGULAR` | 无赋色；`applyShadow: false`（卡片自带阴影） | 跟随系统深浅色，前景走 `$r()` 主题色 |
  | `CONTROL` | 图片上的小控件（应用 poster 的星标 / 动作钮） | `THIN` | 无赋色 | 跟随系统 |
  | `FLOATING` | 会话工具栏 | `THIN` | `materialColor` 着固定深色 | **固定深色，不跟随系统** |
  | `OVERLAY` | Toast、Tips、右键菜单 | `THICK` | 无赋色 | 跟随系统（系统自带前景色） |
  | `DIALOG` | 弹窗（`AlertDialog`、`showConfirm` / `showNotice`） | `ULTRA_THICK` | 无赋色 | 跟随系统 |
  判据（档位随"必须遮住多少背后内容"递增）：承载正文的卡片半通透（`REGULAR`）→ 需要临时压住背后的浮层
  （菜单 / Toast / Tips，`THICK`）→ 需要完全遮住的弹窗（`ULTRA_THICK`）；浮在内容之上、要让内容透出来的表面
  反过来取薄档（`THIN`），其可读性由**赋色**而不是模糊负责——固定的深色工具栏因此用 `THIN`（官方的
  `ULTRA_THIN` 定位在"纯透明浮动栏"，与本工程的固定深色 chrome 不是一回事）。
  - `OVERLAY` 与 `DIALOG` **必须分开**：系统给弹窗的默认档是 `ULTRA_THICK`、给 Toast / 菜单的是 `THICK`，
    而**显式传 `systemMaterial` 会覆盖系统默认**——合成一档就是把弹窗做得比系统默认更透。
  - `FLOATING` 与"浮在画面上的深色 chrome"匹配：模糊只负责"像玻璃"，深浅由 `materialColor` 固定，
    无材质时在 `components/SessionChrome.ets` 的半透明深色底（`TOOLBAR_BG`）上叠**深色档**组件磨砂
    （`colorMode: ThemeColorMode.DARK`）；两条路径都固定深色、不会随系统变浅。
    前景色用该文件的固定深色常量（`TOOLBAR_*` / `METRIC_*`），**不要**改成 `$r('app.color.*')`
    ——那正是"浅底白字"的来源。
  - `CONTROL` **不加 `materialColor`**——材质自身那层磨砂就够形成对比，着色只会把它压平；
    前景色用主题 token 随深浅色走。这类控件是小面积、跟随滚动列表的，材质与磨砂节点数随控件数增长，
    是**有意的成本取舍**。
  - **材质效果不以模拟器为准**：模拟器可能渲染不出材质，小面积控件尤其明显（大面积横栏尚可见）。
    不要据此调样式/取值（会把 `THIN` 误判成"太薄"），材质相关改动只在真机上判定。
- **摆放约束**（直接决定性能与功耗，改挂载点前先看）：
  - **绝不放在远端桌面画面等持续变化的内容上**——材质每帧重新采样背景；
  - 避免大面积、避免嵌套层叠、避免与 `backgroundBlurStyle` 重复（本工程只在**无材质**分支用它做兜底）、
    避免再叠 `shadow`（要保留自己的阴影就把 `applyShadow` 置 `false`）；
  - 滚动列表逐项挂材质、或给整页大容器挂材质，都是已知的坏味道；
    连接列表与设置/编辑页的大卡片属此类，是**有意的成本取舍**，
    面积、数量或滚动频率再增加时必须重新评估。
- **配套**：
  - **底色归材质的互斥分支，不写在调用点**：有材质 → `Color.Transparent`（材质画在底色之下，
    不透明底色会把材质整片盖住），无材质 → 该表面自己的兜底色——跟随系统的表面就是 `Color.Transparent`
    （兜底磨砂自带底色），只有固定深色的 `FLOATING` 才用半透明深色底（`TOOLBAR_BG`）。
    调用点只声明表面；需要固定深色的表面把自己的兜底色交给 modifier，两条分支都由它在这一处决定。
  - `materialColor` 必须是**半透明**色：纯不透明会把材质滤镜整个遮住，只剩一块纯色。
  - `colorInvert` **不能当作对比度保障**：它只在材质足够薄、且系统沉浸光感强度足够时才触发，
    还只对 `$r()` 颜色生效；要保证可读性只能显式配色。
