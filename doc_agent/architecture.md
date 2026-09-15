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
                    原生：FreeRDP 客户端 → GFX 命令 → GPU 引擎 / gdi 回退
```
（配置/密码/设置只在**连接建立时**读取，见 [`settings-and-storage.md`](settings-and-storage.md) §4。）

## 2. ArkTS / ArkUI

| 路径 | 职责 |
|---|---|
| `entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置与连接存储、按默认尺寸创建主窗口、加载连接列表 |
| `sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸（或全屏）创建窗口、加载会话页；单窗口模式下拦截关闭以先恢复主窗口 |
| `pages/Index.ets` | 连接列表（点左侧→编辑，右侧圆钮→连接，右键菜单，拖拽排序，右下角 FAB） |
| `pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏（左侧状态遥测 + 右侧复制/粘贴、全屏/最小化/断开）、全屏状态跟踪 |
| `pages/EditConnectionPage.ets` | 新增/编辑连接（保存只写配置，密码在连接成功后自动保存；连接按钮下方为可折叠「高级设置」） |
| `pages/SettingsPage.ets` | 全局设置（工具栏延迟、RDP 光标、触控板、触屏高刷新率、Win 键替代、全局分辨率/缩放、全局连接特性、窗口尺寸、自动隐藏主窗口、配置导入/导出） |
| `services/ConnectionStore.ets` | 连接配置存储（preferences；稳定 id + `updatedAt`，`useGlobalDisplay`/`scalePercent`/`useGlobalAdvanced`；`reorder` 持久化顺序） |
| `services/CredentialStore.ets` | 密码存储（ASSET，按连接 id，**仅连接成功后写入**） |
| `services/SettingsStore.ets` | 设置存储（preferences）+ 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`、`SCALE_PRESETS`） |
| `services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；密码不导出） |
| `services/SessionManager.ets` | 主窗口后台连接、每连接状态（转圈/已连接/失败）、错误分类、成功后开窗与断连编排 |
| `services/WindowController.ets` | 窗口默认尺寸、拉起独立会话窗口、会话窗口全屏与系统标题栏/dock 控制、单窗口模式的主窗口隐藏/恢复 |
| `services/RdpNative.ets` | 每会话一个实例（独占原生 handle）；按 handle 路由原生事件，`findByKey` 复用 |
| `services/TouchpadWheel.ets` | 触控板 vp 位移 → 高分辨率 RDP 轮转量映射 |
| `services/DeviceCapabilities.ets` | 运行时能力探测（`Capability{supported, reason}`），供 UI 置灰 + 说明原因 |
| `utils/KeyMapper.ets` | 按键名映射（设置页按键框显示用） |
| `model/RdpModels.ets` | 连接/设置的数据模型与默认值 |
| `components/TextAction.ets` | 列表/菜单里的通用文本动作项 |
| `entrybackupability/EntryBackupAbility.ets` | 系统备份/恢复 Ability |
| `pages/GfxReplayPage.ets` | dev 页：码流回放测试（路线选择、统计显示），见 `gfx-engine.md` §6 |

## 3. 原生（`entry/src/main/cpp/`）

| 路径 | 职责 |
|---|---|
| `hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 + 各类查询/开关（音频能力、触屏高刷、RDP 光标、硬件解码…） |
| `hmrdp_session.{h,cpp}` | FreeRDP 客户端生命周期、输入、事件、光标位图、会话遥测；GFX 回调只做解码计时，画面由 gdi 出、经 `VkRenderer` 上屏 |
| `hmrdp_gfx_driver.{h,cpp}` | RDPGFX PDU → 引擎命令的统一映射（实机会话与离线回放共用）；离线回放泵与"逐命令交错 A/B"的链路 |
| `hmrdp_gfx_capture.{h,cpp}` | 原始通道录制（`hmrdp_gfx.bin`）与读取 |
| `hmrdp_gfx_cpu.{h,cpp}` | 离线 FreeRDP gdi 桌面（回放的对比路线）；`PresentGdiFrame` 为 live gdi 回退与 CPU 回放共用 |
| `hmrdp_replay.{h,cpp}` | dev 回放页的引擎侧：把捕获喂给引擎/gdi 并上屏、逐帧对比（见 [`gfx-engine.md`](gfx-engine.md) §6） |
| `hmrdp_vk_context.{h,cpp}` | Vulkan 上下文：`dlopen` + 标准能力探测 + 进程级 instance/device/queue + 内存类型 + 延迟销毁 |
| `hmrdp_vk_renderer.{h,cpp}` | Vulkan 上屏：`VK_OHOS_surface` + swapchain + 缩放/letterbox blit；两条素材来源——引擎屏幕镜像，或 CPU/gdi 帧（`PresentBgraFrame`） |
| `hmrdp_vk_desktop.{h,cpp}` | Vulkan 表面引擎：表面注册表 + 命令执行 + 合成 + 屏幕脏区（host-visible 缓冲存储） |
| `shaders/*.comp` + `cmake/EmbedSpirv.cmake` | GLSL → SPIR-V 的构建期编译/嵌入 |
| `hmrdp_audio.{h,cpp}` | `dlopen` OHAudio 的 PCM 播放器（见 [`native-libraries.md`](native-libraries.md) §5） |
| `hmrdp_log.h` | hilog 包装（domain `0xD001`、tag `HmRdpNative`） |
| `hmrdp_rfx.{h,cpp}` | Progressive 容器解析器（`ParseRfxProgressive`）+ 共享命令模型（`GpuSurface`/`GpuCmd`/`GpuCodec`）+ ClearCodec hook |

## 4. 其它

- `native/scripts/*` —— 原生库构建与 FreeRDP 补丁（见 [`native-libraries.md`](native-libraries.md)）。
- `entry/libs/<abi>/` —— 本地构建的原生库（**不入库**）。
- `resources/base|dark/` —— 主题色与图标。
