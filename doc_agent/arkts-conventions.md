# ArkTS / ArkUI 约定

## 1. 严格 ArkTS（编译期硬约束）

- 禁用 `any` / `unknown`；
- 除受支持形式外禁用 `as` 类型断言；
- 禁用结构化类型、解构赋值、**未标注类型的对象字面量**（对象字面量必须有显式类型上下文）；
- `async` 必须显式标注 `Promise<T>`。

## 2. 状态与刷新（最常踩的运行时坑）

- **`@Builder` 的参数按值传递时不会随状态刷新**：需要响应状态变化的 builder 必须传
  **单个对象字面量**（按引用），并在 builder 内访问其属性。
- **`ForEach` 的 key 必须随内容变化**：否则列表项会被复用、`onClick` 闭包仍持有旧对象，
  UI 与连接动作会用到过期数据。本项目统一用 `${conn.id}#${conn.updatedAt}` 作 key
  （`updatedAt` 每次保存都刷新，就是为了让 key 变化）。
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

## 5. 与原生交互

- 每个会话窗口一个 `RdpNative` 实例（独占原生 handle）；原生事件带 handle，按 handle 路由；
  `findByKey` 按会话 key 复用实例。
- 原生侧能力差异（音频、GPU 加速等）走 `services/DeviceCapabilities.ets` 的
  `Capability{supported, reason}`：UI 置灰 + 说明原因，不要静默失败（见
  [`native-libraries.md`](native-libraries.md) §6）。
- 设置项、连接字段、原生开关三者的对应关系见 [`settings-and-storage.md`](settings-and-storage.md)。
