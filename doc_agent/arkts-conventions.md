# ArkTS / ArkUI 约定

## 1. 严格 ArkTS（编译期硬约束）

- 禁用 `any` / `unknown`；
- 除受支持形式外禁用 `as` 类型断言；
- 禁用结构化类型、解构赋值、**未标注类型的对象字面量**（对象字面量必须有显式类型上下文）；
- `async` 必须显式标注 `Promise<T>`。

## 2. 状态与刷新（最常踩的运行时坑）

- **`@Builder` 的参数按值传递时不会随状态刷新**：需要响应状态变化的 builder 必须传
  **单个对象字面量**（按引用），并在 builder 内访问其属性。
- **`@BuilderParam` 的初始化方式决定 `this` 指向（运行期崩溃）**：以 `this.xxx` **直接引用**把父组件 `@Builder`
  传给子组件 `@BuilderParam` 时，`this` 会变成**子组件实例**，builder 内 `this.someMethod(...)` 于是抛
  `TypeError: undefined is not callable`（编译期不报，点击/交互时才崩）。两种安全写法：**借用箭头函数**
  `Child({ content: () => { this.someBuilder(); } })`（箭头 `this` 绑定父组件）；或**尾随闭包**
  `Child({ ... }) { ... }`（内容按 `@Builder` 传给唯一的 `@BuilderParam`，`this` 绑定父组件）——尾随闭包要求
  子组件**有且仅有一个无参 `@BuilderParam`，且该组件不挂通用属性**。否则把内容做成独立子组件 / 显式参数。
- **`ForEach` 项里渲染会变的状态，正解是「子组件 + `@Prop`」，不是堆 key**：列表项写成**自定义组件**，
  把会变的状态用 `@Prop` 传入——父组件重渲染时 `@Prop` 会**在原地更新**（`ConnectionRow` 的 `state`
  就是这样驱动连接钮的加载态）。**不要**用页面 `@Builder` 内联整个列表项：`@Builder` 按值传参不刷新
  （见下条），项会被冻住（点星标、连上会话、图标到位全都不动）。
- key **只需随「身份数据」变化**：本项目统一用 `${conn.id}#${conn.updatedAt}`（`updatedAt` 每次保存都刷新）。
  应用管理页的 tile（`components/MoonlightAppTile`）因 **图标位图 `PixelMap` 无法走 `@Prop`**（`@Prop`
  深拷贝会丢 NAPI 复杂类型的类型），图标走**普通字段**、key 用 `${app.id}#${iconMap.size}`，只在图标
  到位时重建该 tile；默认 / 正在运行 / 会话态一律走 `@Prop`。
- **`@Builder` 按值传参不会刷新**（官方 FAQ：私有/全局 Builder 按值传递的参数改变都不刷新 UI）——
  **这条在自定义组件内部同样成立**：组件因 `@Prop` 重渲染时，`build()` 里的 `if` 条件会重算（所以
  「按钮出现」正常），但经按值 `@Builder` 传下去的属性（如星标颜色）依旧不刷新。要动态刷新只有：
  传**单个对象字面量**（按引用——单参、必须是对象字面量、类型已声明，条件苛刻；应用管理页的
  `controlDisc` 即此写法），或改用「按回调传参」，或**包一层自定义组件**（`ChoiceChip` 随选中态刷新）。
- **`JSON.parse` 反序列化**：`as` 只影响编译期，**不会**转换成员类型，也**不会**恢复类默认值。
  统一 `JSON.parse(text) as Record<string, Object>` 再逐字段读取（见 `ConfigTransfer.readSettings`）；
  不要直接 `as` 成业务类后依赖其字段类型或默认值。

## 3. 路由

- 路由参数必须是**具名接口**（如 `EditConnectionParams`），通过 `this.getUIContext().getRouter()` 传递。
- 页面间不传密码等敏感值：会话窗口只传**会话 key**（见 [`session-and-input.md`](session-and-input.md) §1）。

## 4. 主题与资源

- 主题色在 `resources/base|dark/element/color.json`，代码里用 `$r('app.color.*')`；深浅色跟随系统。
- 需要**深色变体**的 SVG 图标放 `resources/dark/media/`（同名文件覆盖 base）。
- 文案/尺寸尽量走资源与常量，不要在多个页面里各写一份。
- 各 token 的**用途**、尺寸 / 圆角 / 字号取值与组件规范见 [`ui-design.md`](ui-design.md)；本节只讲资源与主题机制。

## 5. 与原生交互

- 每个会话窗口一个会话后端 `SessionBackend`（RDP 实现 `RdpSessionBackend` 内部独占一个 `RdpNative` 实例与原生
  handle）；原生事件带 handle，按 handle 路由；`SessionManager` 按 `sessionKey` 经 `SessionBackendRegistry` 关联窗口与后端。
- 原生侧能力差异（音频、GPU 加速等）走 `services/DeviceCapabilities.ets` 的
  `Capability{supported, reason}`：UI 置灰 + 说明原因，不要静默失败（见
  [`native-libraries.md`](native-libraries.md) §6）。
- 设置项、连接字段、原生开关三者的对应关系见 [`settings-and-storage.md`](settings-and-storage.md)。

## 6. 沉浸光感（系统材质）

ArkUI 的系统材质 `uiMaterial` 自 **API 26.0.0** 起提供。材质等级由**设备算力**决定，效果强弱由
**系统"沉浸光感"设置**决定；两者都由系统自适应，应用**没有接口读写也不该去读写**——
应用只决定"哪些表面挂材质"。统一入口是 `services/SystemMaterial.ets`。

- **门槛判断**（三者都过才返回材质，否则返回 `undefined`，调用点保留原本的平色背景）：
  1. `deviceInfo.sdkApiVersion >= 26`：老系统上 `deviceInfo.apiAvailable()` **自身不存在**，
     只能先用底座版本号兜住；
  2. `if (deviceInfo.apiAvailable('26.0.0'))`：版本号必须是**字面量、直接写在 if 里**
     （不支持封装、变量、三元或 `&&`/`||`），编译器据此消除 `compatibleSdkVersion` 的兼容性告警；
  3. `uiMaterial.isImmersiveMaterialSupported()`。
- **两种挂法**，取决于组件如何暴露该属性：
  - **通用属性 `systemMaterial`** → 用 `ImmersiveModifier`（`AttributeModifier`）施加。
    该属性 API 26 才存在，直接内联写在组件上会让老系统的整棵组件构建失败（不是降级）；
    组件类型不同，`AttributeModifier<T>` 不通用，按组件属性类型实例化。
  - **options 对象**（Toast、Tips、右键菜单、AlertDialog/`showAlertDialog`）→ 作为字段传入，
    老系统读到它只会忽略，无需额外包装。
- **材质对象建一次复用**：改参数会触发系统重算材质，每次渲染新建会让材质层一直重绘。
- **表面清单与配色方向**（`SystemMaterial.MaterialSurface`）：
  - `CARD`（连接 / 设置 / 编辑页卡片、连接状态卡片）：**跟随系统深浅色**。材质在浅色模式下是浅色的，
    所以前景色与兜底底色都必须走 `$r()` 主题色，否则浅色模式下是"浅底 + 白字"。
  - `OVERLAY`（Toast、Tips、右键菜单、AlertDialog）：同上（系统自带前景色，无需额外配色）。
  - `FLOATING`（会话工具栏、状态遮罩）：**故意固定深色，不跟随系统**。它浮在远端桌面画面上，背后颜色不可知，
    浅色 chrome 会随远端内容变得不可读；因此材质用 `materialColor` 着深色、不支持材质时兜底为深色，
    前景色用 `components/SessionChrome.ets` 的固定深色常量（`TOOLBAR_*` / `METRIC_*`）。
    **不要**把这些改成 `$r('app.color.*')`——那正是"浅底白字"的来源。
  - `CONTROL`（图片上的小控件，如应用 poster 上的星标 / 动作钮）：`THIN`（官方对悬浮按钮的建议样式），
    **不加 `materialColor`**——材质自身那层磨砂就够形成对比，着色只会把它压平；前景色用主题 token
    随深浅色走。系统没有材质能力时用组件级磨砂 `backgroundBlurStyle(BlurStyle.COMPONENT_THIN)` 兜底。
    **该模糊必须只写在「无材质」分支，不能与材质同时写在组件上**：`backgroundBlurStyle` 的视觉层级高于材质层，
    会把材质整片盖掉（连设成 `NONE` 也会建这一层，一样盖住），所以是"有材质就完全不设模糊"，而不是"设成 NONE"。
    这类控件是小面积、跟随滚动列表的，材质节点数随控件数增长，是**有意的成本取舍**。
  - **材质效果不以模拟器为准**：模拟器可能渲染不出材质，小面积控件尤其明显（大面积横栏尚可见）。
    不要据此调样式/取值（会把 `THIN` 误判成"太薄"），材质相关改动只在真机上判定。
- **摆放约束**（直接决定性能与功耗，改挂载点前先看）：
  - **绝不放在远端桌面画面等持续变化的内容上**——材质每帧重新采样背景；
  - 避免大面积、避免嵌套层叠、避免与 `backgroundBlurStyle` 重复、避免再叠 `shadow`
    （要保留自己的阴影就把 `applyShadow` 置 `false`）；
  - 滚动列表逐项挂材质、或给整页大容器挂材质，都是已知的坏味道；
    连接列表与设置/编辑页的大卡片属此类，是**有意的成本取舍**，
    面积、数量或滚动频率再增加时必须重新评估。
- **配套**：
  - 设了材质后背景色要显式置 `Color.Transparent`，否则不透明底色盖住材质；
    不支持材质时又要退回兜底色，所以两者写在同一个三元表达式里。
  - `materialColor` 必须是**半透明**色：纯不透明会把材质滤镜整个遮住，只剩一块纯色。
  - `colorInvert` **不能当作对比度保障**：它只在材质足够薄、且系统沉浸光感强度足够时才触发，
    还只对 `$r()` 颜色生效；要保证可读性只能显式配色。
