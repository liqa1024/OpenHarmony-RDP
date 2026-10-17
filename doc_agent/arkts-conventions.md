# ArkTS / ArkUI 约定

## 1. 严格 ArkTS（编译期硬约束）

- 禁用 `any` / `unknown`；
- 除受支持形式外禁用 `as` 类型断言；
- 禁用结构化类型、解构赋值、**未标注类型的对象字面量**（对象字面量必须有显式类型上下文）；
- `async` 必须显式标注 `Promise<T>`。

## 2. 状态与刷新（最常踩的运行时坑）

- **`@Builder` 的参数按值传递时不会随状态刷新**：需要响应状态变化的 builder 必须传**单个对象字面量**
  （按引用），并在 builder 内访问其属性。**这条在自定义组件内部同样成立**——组件因 `@Prop` 重渲染时，
  `build()` 里的 `if` 条件会重算（所以「按钮出现」正常），但经按值 `@Builder` 传下去的属性（如某个 `@Prop` 值）
  依旧不刷新。要动态刷新只有：传单个对象字面量（按引用）、改用「按回调传参」、或**包一层自定义组件**。
- **`@BuilderParam` 的初始化方式决定 `this` 指向（运行期崩溃）**：以 `this.xxx` **直接引用**把父组件 `@Builder`
  传给子组件 `@BuilderParam` 时，`this` 会变成**子组件实例**，builder 内 `this.someMethod(...)` 于是抛
  `TypeError: undefined is not callable`（编译期不报，点击/交互时才崩）。两种安全写法：**借用箭头函数**
  `Child({ content: () => { this.someBuilder(); } })`（箭头 `this` 绑定父组件）；或**尾随闭包**
  `Child({ ... }) { ... }`（内容按 `@Builder` 传给唯一的 `@BuilderParam`，`this` 绑定父组件）——尾随闭包要求
  子组件**有且仅有一个无参 `@BuilderParam`，且该组件不挂通用属性**。否则把内容做成独立子组件 / 显式参数。
- **`ForEach` 项里渲染会变的状态，正解是「子组件 + `@Prop`」，不是堆 key**：列表项写成**自定义组件**，
  把会变的状态用 `@Prop` 传入——父组件重渲染时 `@Prop` 会**在原地更新**（如列表项的连接状态驱动按钮文案）。
  **不要**用页面 `@Builder` 内联整个列表项：`@Builder` 按值传参不刷新，
  项会被冻住（点按钮、连上会话、异步数据到位全都不动）。
- key **只需随「身份数据」变化**：本项目统一用 `${conn.id}#${conn.updatedAt}`（`updatedAt` 每次保存都刷新）。
  列表项里若含 **`PixelMap` 位图**（如应用图标），不要走 `@Prop`（`@Prop` 深拷贝会丢 NAPI 复杂类型的类型），
  位图走**普通字段**、key 里带上"该图是否就绪"；不要用整张图标表的 `size` 当 key——
  每到位一张图就变，会让**所有**项一起重建、并重放出现转场。
- **`JSON.parse` 反序列化**：`as` 只影响编译期，**不会**转换成员类型，也**不会**恢复类默认值。
  统一 `JSON.parse(text) as Record<string, Object>` 再逐字段读取（见 `ConfigTransfer.readSettings`）；
  不要直接 `as` 成业务类后依赖其字段类型或默认值。

## 3. 路由

- 路由参数必须是**具名接口**（如 `EditConnectionParams`），通过 `this.getUIContext().getRouter()` 传递。
- 页面间不传密码等敏感值：会话窗口只传**会话 key**（见 [`session-layer.md`](session-layer.md) §5）。

## 4. 资源与主题机制

- 主题色与图标资源在 `resources/base|dark`；代码里用 `$r('app.color.*')`，深浅色跟随系统。
- 需要深色变体的 SVG 放在 `resources/dark/media/`（同名文件覆盖 base）。
- 具体颜色、尺寸、材质取值不在文档规定，由视觉层决定。

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


