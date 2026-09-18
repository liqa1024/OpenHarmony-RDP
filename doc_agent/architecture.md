# 应用结构

## 1. 一句话数据流

```
连接列表(Index) ──选中──> SessionManager（主窗口后台连接）
                              │  解析显示/高级设置：SettingsStore.resolveDisplay / resolveAdvanced
                              │  取密码：CredentialStore(ASSET)
                              │  开窗：WindowController → SessionAbility（独立窗口，instanceKey）
                              ▼
                        SessionPage（XComponent 画面 + 输入 + 工具栏/遥测）
                              │  每会话一个 RdpNative 实例（Node-API → hmrdp_session）
                              ▼
                    原生：FreeRDP 客户端 → GFX → gdi 解码 → 呈现器上屏（Vulkan/GLES）
```

配置/密码/设置只在**连接建立时**读取，见 [`settings-and-storage.md`](settings-and-storage.md) §4。

## 2. ArkTS / ArkUI

| 路径 | 职责 |
|---|---|
| `entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置与连接存储、按默认尺寸创建主窗口、加载连接列表 |
| `sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸（或全屏）创建窗口、加载会话页；单窗口模式下拦截关闭以先恢复主窗口 |
| `pages/Index.ets` | 连接列表（点左侧→编辑，右侧圆钮→连接，右键菜单，拖拽排序，FAB 新建） |
| `pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏、全屏状态跟踪 |
| `pages/EditConnectionPage.ets` | 新增/编辑连接（保存只写配置；密码在连接成功后自动保存；可折叠「高级设置」） |
| `pages/SettingsPage.ets` | 全局设置（工具栏、光标、触控板/触屏、Win 键替代、分辨率与缩放、连接特性、窗口尺寸、主窗口自动隐藏、配置导入/导出） |
| `services/ConnectionStore.ets` | 连接配置存储（preferences；稳定 id + `updatedAt`；`reorder` 持久化顺序） |
| `services/CredentialStore.ets` | 密码存储（ASSET，按连接 id，**仅连接成功后写入**） |
| `services/SettingsStore.ets` | 设置存储（preferences）+ 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`） |
| `services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；密码不导出） |
| `services/SessionManager.ets` | 主窗口后台连接、每连接状态、错误分类、成功后开窗与断连编排 |
| `services/WindowController.ets` | 窗口默认尺寸、拉起会话窗口、全屏与系统标题栏控制、单窗口模式的主窗口隐藏/恢复 |
| `services/RdpNative.ets` | 每会话一个实例（独占原生 handle）；按 handle 路由原生事件，`findByKey` 复用 |
| `services/TouchpadWheel.ets` | 触控板 vp 位移 → 高分辨率 RDP 轮转量映射 |
| `services/DeviceCapabilities.ets` | 运行时能力探测（`Capability{supported, reason}`），供 UI 置灰 + 说明原因 |
| `utils/KeyMapper.ets` | 按键名映射（设置页按键框显示用） |
| `model/RdpModels.ets` | 连接/设置的数据模型与默认值 |
| `components/TextAction.ets` | 列表/菜单里的通用文本动作项 |
| `entrybackupability/EntryBackupAbility.ets` | 系统备份/恢复 Ability |
| `pages/GfxReplayPage.ets` | dev 页：码流回放测试（路线选择、统计显示），见 [`gfx-engine.md`](gfx-engine.md) §6 |

## 3. 原生（`entry/src/main/cpp/`）

| 路径 | 职责 |
|---|---|
| `hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 + 各类查询/开关 |
| `hmrdp_session.{h,cpp}` | FreeRDP 客户端生命周期、输入、事件、光标位图、会话遥测；画面由 gdi 出、经呈现器上屏 |
| `hmrdp_gfx_driver.{h,cpp}` | 原始 GFX 通道的离线回放泵（FreeRDP 自己的 ZGX + RDPGFX 解析）与解析计时 |
| `hmrdp_gfx_capture.{h,cpp}` | 原始通道录制与读取 |
| `hmrdp_gfx_cpu.{h,cpp}` | 离线 FreeRDP gdi 桌面（回放的解码器）；`PresentGdiFrame` 为 live 与回放共用 |
| `hmrdp_presenter.{h,cpp}` | **呈现器接口 + 后端选择**：按呈现能力选定 Vulkan 或 GLES |
| `hmrdp_gles_presenter.{h,cpp}` | **GLES/EGL 兜底呈现器**：脏矩形 `glTexSubImage2D` + letterbox quad |
| `hmrdp_replay.{h,cpp}` | dev 回放页：喂流、上屏、参考画面对比（见 [`gfx-engine.md`](gfx-engine.md) §6） |
| `hmrdp_vk_context.{h,cpp}` | Vulkan 上下文：`dlopen` + 标准能力探测 + 进程级 instance/device/queue + 内存类型 + 延迟销毁 |
| `hmrdp_vk_renderer.{h,cpp}` | Vulkan 上屏：`VK_OHOS_surface` + swapchain + letterbox quad + 零拷贝桌面缓冲 |
| `shaders/present_quad.*` + `cmake/EmbedSpirv.cmake` | GLSL → SPIR-V 的构建期编译/嵌入 |
| `hmrdp_audio.{h,cpp}` | `dlopen` OHAudio 的 PCM 播放器（见 [`native-libraries.md`](native-libraries.md) §5） |
| `hmrdp_log.h` | hilog 包装（domain `0xD001`、tag `HmRdpNative`） |

## 4. 其它

- `native/scripts/*` —— 原生库构建与 FreeRDP 补丁（见 [`native-libraries.md`](native-libraries.md)）。
- `entry/libs/<abi>/` —— 本地构建的原生库（**不入库**）。
- `resources/base|dark/` —— 主题色与图标。
