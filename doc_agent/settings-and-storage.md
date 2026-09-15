# 设置 / 存储 / 密码 / 导入导出

## 1. 存储分工

| 数据 | 位置 | 说明 |
|---|---|---|
| 全局设置 | `SettingsStore`（preferences） | 显示、连接特性默认值、窗口、输入等 |
| 连接配置 | `ConnectionStore`（preferences） | **稳定 UUID 作 id** + `updatedAt`（供 UI 刷新/作 key） |
| 密码 | `CredentialStore`（**ASSET**） | 按连接 id 存，**只在连接验证成功后写入** |
| 导出文件 | 用户选择位置（`DocumentViewPicker` + `fileIo`） | JSON，**不含密码** |

- **密码绝不落明文、不进日志、不经命令行传递**：原生侧用 `freerdp_settings_set_string` 设
  `FreeRDP_Password` / `FreeRDP_GatewayPassword`。
- preferences 的字符串字段**先 `encodeURIComponent` 再拼接**，否则控制字符会损坏文件。

## 2. 设置模型：全局默认 + 单连接覆盖

- **显示（分辨率/缩放）**：全局可自动（取当前显示器 + 推荐缩放档位
  `100/125/150/175/200/225`）或手动；单连接可关掉「使用全局显示设置」用自己保存的 `width/height/scalePercent`。
  连接前用 `SettingsStore.resolveDisplay(conn)` 解析。
  - 自动缩取的来源是 `densityPixels × 100` 并**吸附**到档位（`SCALE_PRESETS`）；
  - 原生只写 `FreeRDP_DesktopScaleFactor`（缩放交给服务端，客户端不做逐像素缩放）。
- **高级连接特性（音频、忽略证书…）**：默认值放全局 `AppSettings`；单连接保存自己的值 +
  `useGlobalAdvanced` 标志（**默认跟随全局**）。连接前用 `SettingsStore.resolveAdvanced(conn)` 解析。
  编辑页关掉「使用全局高级设置」后才用本连接的独立开关；**保存时仍持久化独立值**，便于随时切回。
- **剪贴板**固定开启（全局开关已移除），但保留单连接的「剪贴板重定向」开关可单独关闭。
- **GFX 恒开**：关掉图形管线会让会话完全不可用，故其开关已删除；`Connect` 里无条件置
  `FreeRDP_SupportGraphicsPipeline`（`AppSettings.enableGfx` 字段保留但已不生效）。
- **硬件解码**（GPU 加速）是**真机专属**，见 [`gfx-engine.md`](gfx-engine.md)；**当前不生效**（没有引擎接进
  live，见下）。

## 3. 导入 / 导出（`ConfigTransfer`）

- 导出内容 = 全局设置 + 全部连接；**密码不导出**（在 ASSET 中），UI/弹窗必须提示"导入后需重新输入"。
- 导出用**独立的 DTO**（`ExportedConnection`）而不是直接序列化 `SavedConnection`：否则会把继承来的
  `password` / `gatewayPassword` 字段带出去。
- 导入按连接 **`id` 覆盖或新增**，并刷新 `updatedAt`（保证 UI key 变化）。入口在设置页底部。
- 解析导出文件时统一 `JSON.parse(text) as Record<string, Object>` 后逐字段读取（原因见
  [`arkts-conventions.md`](arkts-conventions.md) §2）。

## 4. 生效时机（**改设置前必读**）

- 所有连接相关设置（分辨率/缩放、连接特性、触屏高刷新率、使用 RDP 光标等）在**连接建立时读取**：
  **改动后需要重开会话才生效**，已打开的会话不受影响。
- 窗口尺寸类设置只在**窗口创建时**生效。
- 回放/抓取这类 dev 开关在**下一次回放开始时**读取（页面切换路线会重启回放）。
- 「硬件解码（RFX）」当前**不参与决策**：没有引擎接进 live 会话（Vulkan 引擎目前只用于回放/对比），
  因此会话一律走 gdi 软解；该设置保留给 Vulkan 接管 live 时使用。**注意它与呈现无关**：gdi 帧照样经
  Vulkan/GLES 呈现器上屏（两套能力判定独立）。设置页会**按设备能力置灰**它
  （`DeviceCapabilities.hardwareDecode()`：无 Vulkan / 无 compute 队列 / 无 host-visible 内存 /
  缺上屏扩展 / 模拟器 ⇒ 不支持并显示原因，同时把已存的值纠正为关）。
