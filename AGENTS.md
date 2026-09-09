# AGENTS.md

供 AI 编码助手与贡献者阅读的工程指南。项目：**RDP 远程桌面**（代号 **HmRdp**）。

## 项目定位

面向 **鸿蒙 PC（2in1）** 的 RDP 客户端。RDP 引擎为 FreeRDP 3.10.3，从源码交叉编译到
`aarch64-linux-ohos` / `x86_64-linux-ohos`。界面为 ArkTS/ArkUI；画面通过 XComponent 上的
EGL/GLES 原生渲染；输入经 Node-API 桥接转发。会话在独立的 `SessionAbility` 主窗口中打开；
主窗口 / 会话窗口的默认尺寸按屏幕比例推导，会话分辨率/缩放默认自适应当前显示器，均可在
全局设置中调整，单个连接也可在「高级设置」里覆盖。全局设置还可开启「自动隐藏主窗口」
（单窗口模式，仅单会话：会话窗口打开时销毁主窗口，关闭后恢复，见第 12 条）。

- 应用名：**RDP 远程桌面** · Bundle：`com.lixa.hmrdp` · 目标：HarmonyOS 6.1.0（API 23）

## 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 位于 `sdk/default/openharmony`）。
- 应用 `compatibleSdkVersion` / `targetSdkVersion`：`6.1.0(23)` —— 必须与目标设备/模拟器
  （HarmonyOS 6.1.0）一致，未经确认不要擅自调高。
- ABI：`arm64-v8a`（真机）、`x86_64`（模拟器）。
- 宿主为 Windows；OpenSSL 在 WSL 中编译，驱动 Windows 版 OHOS NDK 的 `clang.exe`。

## 构建 / 运行 / 验证

```bash
devecocli build                                   # 仅编译
devecocli run --device "<模拟器名/序列号>"        # 编译 + 签名 + 安装 + 启动
```

- 改动 `.ets` 后先跑 `arkts_check`，再跑 `build_project`。
- 任务结束前 `build_project` 必须通过。
- 仓库**不含签名材料**。本地需配置 DevEco 自动签名，或运行 `devecocli signature generate`。
  切勿提交 `*.p12/p7b/cer` 或 keystore。

### 模拟器（功能测试）

- 首选模拟器：**<2in1 emulator>**（2in1，HarmonyOS 6.1.0，x86_64）。
- `devecocli emulator start "<2in1 emulator>"`。
- `uitest uiInput` 注入的是**触摸**事件，不会触发 `onMouse`；鼠标请用 `uinput -M ...`，
  但其 `-m` 是**相对/累加**移动且指针常"不可见"，精确定位不可靠。
- 精确坐标：`uitest dumpLayout -p /data/local/tmp/layout.json` + `hdc file recv`，按控件
  `bounds` 计算 `uitest uiInput click <x> <y>` 的中心点（比目测截图可靠）。
- 截图：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg` + `hdc file recv`。
- 连接多个设备时 `hdc` 需用 `-t <serial>` 指定目标。

## 原生库源码构建（可选；预编译库已提交）

```
native/scripts/patch-freerdp.ps1    # FreeRDP 的 OHOS 补丁（musl pthread_cancel、OpenSLES、client-common SHARED）
native/scripts/build-openssl-wsl.sh # OpenSSL，在 WSL 中运行，驱动 Windows OHOS clang
native/scripts/build-freerdp.ps1    # FreeRDP 的 CMake 构建（Windows NDK）
```

`native/third_party/`、`native/build/`、`native/install/`、`native/tools/` 已 gitignore。
产出的 `.so` 复制到 `entry/libs/<abi>/`，这些**要提交**。

> **必须保持 `-DWITH_VERBOSE_WINPR_ASSERT=OFF`**（见 `build-freerdp.ps1`）。默认 ON 时
> `WINPR_ASSERT` 会 `abort()` 整个进程：OHOS 上 OpenSLES 打不开音频设备时
> `rdpsnd_opensles_open` 的 `WINPR_ASSERT(opensles->stream)` 会直接闪退。关闭后
> `WINPR_ASSERT` 退化为被 `NDEBUG` 禁用的 `assert()`，音频后端失败时走错误分支静默降级，
> 不再拖垮应用。改回 ON 前务必先解决音频设备打开失败的问题。
>
> **rdpsnd 的 OpenSLES 后端被替换为 `native/patches/rdpsnd_opensl_io.{c,h}`**（见
> `patch-freerdp.ps1` 第 4 步）。OHOS 只实现了 `SL_IID_OH_BUFFERQUEUE`（拉模型，回调里
> `GetBuffer`→填充→`Enqueue`），不支持上游用的 `SL_IID_BUFFERQUEUE`；不替换时
> `CreateAudioPlayer` 会失败、音频无声。已用应用内探针确认：引擎/输出混音/播放器/PLAY/
> VOLUME/OH_BUFFERQUEUE 全部返回 `SL_RESULT_SUCCESS`。

## 关键实现要点（改动前必读）

1. **GFX 管线**：`HmrdpPreConnect` 必须把 `ChannelConnected`/`ChannelDisconnected` 订阅到
   `freerdp_client_OnChannelConnectedEventHandler`，否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
2. **鼠标按键**：`PTR_FLAGS_MOVE` 与按键事件分开送，按键事件不带 MOVE 标志，否则远端忽略点击。
3. **密码**：绝不经命令行传密码，用 `freerdp_settings_set_string` 设 `FreeRDP_Password` /
   `FreeRDP_GatewayPassword`。
4. **帧上传**：每次上传整帧（`glTexSubImage2D`）；按脏区部分上传会花屏，除非改用 PBO/EGL image。
5. **原生库命名**：`entry/libs/<abi>/` 下同时放 `libX.so`（链接期）与 `libX.so.3`（运行期）。
6. **连接与密码存储**：配置存 `ConnectionStore`（preferences，稳定 UUID 作 id、`updatedAt` 供刷新），
   密码单独存 `CredentialStore`（ASSET，按 id），**只在连接成功后**写入。preferences 字符串字段先
   `encodeURIComponent` 再拼接，否则控制字符会损坏文件。
7. **XComponent 输入**：surface 用 `surfaceId` + `OH_NativeWindow_CreateNativeWindowFromSurfaceId`
   （不带 `libraryname`），输入走 ArkUI `onMouse/onTouch/onKeyEvent/onAxisEvent`。
8. **窗口尺寸**：默认按屏幕 × 45%（主窗）/ 67%（会话窗），屏幕查询失败则不改窗口；尺寸设置只在
   **窗口创建时**生效，`resize`/`moveWindowTo` 单位是 **px**（非 vp）。`sessionFullscreen` 经
   `StartOptions.windowMode = WINDOW_MODE_FULLSCREEN` 直接全屏；会话窗的**系统最大化**在
   `WindowController.setupSessionWindow` 里转成沉浸式全屏（隐藏标题栏/dock 悬停）。
9. **会话窗口**：用独立 `SessionAbility`（`launchType: specified` + 唯一 `instanceKey`）才有完整标题栏
   （子窗没有最小化按钮）。每次连接生成 `sessionKey`，`PendingConnection` 放 `SessionRequests`、Want
   只带 key（密码不进 Want）；**主窗口后台连接，成功后才开窗**，失败只报错不开窗。每个会话独占一个
   `RdpNative` 实例，原生事件带 handle、按 handle 路由。关闭窗口即断连：
   `SessionAbility.onWindowStageDestroy/onDestroy` → `SessionManager.release` + `SessionRequests.discard`
   （`SessionPage.aboutToDisappear` 在窗口关闭时**不触发**，别依赖它断连）。连接有 30s 超时兜底，
   错误经 `SessionManager.describeError` 分类（原生格式 `<错误码>|<消息>`）。
10. **输入分流**：鼠标→`onMouse`、真触屏→`onTouch`（RDPEI 触屏）、滚轮/触控板→`onAxisEvent`；用
    `event.source === SourceType.TouchScreen` 区分真触屏与鼠标转成的触摸，避免点击变拖动。触屏手指
    id 要 `+1`（FreeRDP 用 `id == 0` 表示空槽）。触控板双指滚动单位是像素、方向与滚轮相反，横向映射
    `HWHEEL`；捏合映射为 Ctrl+滚轮（全程按住 Ctrl）。别用 `easy_go.json` 的 `mouse2TouchEventMode`
    关鼠标转触摸（本机 SDK schema 不含该字段，hvigor 校验失败）。
11. **分辨率与缩放**：自动分辨率取显示器宽高，缩放取 `densityPixels × 100` 并吸附到
    100/125/150/175/200/225（`SettingsStore.SCALE_PRESETS`）；经 `RdpOptions.scalePercent` → 原生只写
    `FreeRDP_DesktopScaleFactor`。每个连接可关「使用全局显示设置」用自己保存的
    `width/height/scalePercent`；连接前用 `SettingsStore.resolveDisplay(conn)` 解析。
14. **高级连接特性（全局默认 + 单连接覆盖）**：剪贴板/音频/GFX/H.264/忽略证书这 5 项默认值放在
    `AppSettings`（设置页「连接特性」区），连接保存自己的独立值 + `useGlobalAdvanced` 标志，
    **默认跟随全局**（`SavedConnection.useGlobalAdvanced = true`）。连接前用
    `SettingsStore.resolveAdvanced(conn)` 解析，再写入 `RdpConnectOptions`。编辑页「高级设置」里
    「使用全局高级设置」关掉后才会用本连接的独立开关；保存时仍持久化独立值，便于随时切回。
15. **剪贴板重定向（文本）**：原生在 `ChannelConnected` 里捕获 `CliprdrClientContext`（`HmrdpChannelConnected`
    先调用默认 handler 再判断 `cliprdr`），只覆写 `Server*` 回调与 `MonitorReady`，**不要动
    `Client*` 发送函数**。文本按 CF_UNICODETEXT 传输，本地文本缓存为 NUL 结尾 UTF-16LE。
    - **远端→本机**：收到原生 `RdpEvent.ClipboardText`（值为 4）时 `SessionPage` 写回系统剪贴板
      （`setData` 不需要权限）。
    - **本机→远端**：`ClipboardSync` 监听系统剪贴板 `update` 事件，变化时自动推送到远端
      （`native.setClipboard(text)`），因此需要申请 `ohos.permission.READ_PASTEBOARD`
      （`module.json5` 声明 + `EntryAbility` 运行时申请）。**该权限为 `system_basic` 级，普通签名
      真机可能无法授权，届时本机→远端会静默失效；权限方案待后续解决**（曾考虑 PasteButton 安全
      控件，当前未采用）。
    - 仅支持纯文本，图片/文件不处理。
12. **自动隐藏主窗口（单窗口模式）**（`AppSettings.autoHideMainWindow`，默认关）：开启后仍**新建**
    `SessionAbility` 会话窗，但**销毁主 `EntryAbility`** 以真正隐藏（无 hide API，`minimize()` 仍在
    Dock）；按单会话设计，故 `WindowController` 只用 `mainHidden` 布尔量，不跟踪 session 集合。
    - **进程内最后一个 UIAbility 被销毁 → 进程退出**：必须等会话窗加载完成（`onSessionWindowReady`）
      后才 `terminateSelf()` 主窗口，否则 `Terminate last` 会杀掉整个应用。
    - 关闭会话时先 `startAbility(EntryAbility)` 拉起主窗口，`onMainWindowReady` 后再终止会话；用
      `windowStage.on('windowStageClose')` + `UIAbility.onPrepareToTerminate()` 拦截关闭（本机模拟器
      后者不触发），`closeSession` 去重 + 3s 超时兜底。关闭该选项则行为不变（多窗口可并存）。
13. **配置导入/导出**：`ConfigTransfer` 把全局设置 + 全部连接导出为 JSON（`picker.DocumentViewPicker`
    选择文件/路径，`fileIo` 读写）。密码在 ASSET 中，**不导出**，UI 与弹窗需提示「导入后需重新输入」。
    导出用独立的 `ExportedConnection` DTO 而非直接序列化 `SavedConnection`，否则会带出继承的
    `password`/`gatewayPassword` 字段；导入按连接 `id` 覆盖/新增并刷新 `updatedAt`。入口在设置页底部。

## ArkTS 规范

- 严格 ArkTS：禁用 `any`/`unknown`，除受支持形式外禁用 `as`，禁用结构化类型、解构、
  未标注类型的对象字面量；async 必须显式标注 `Promise<T>`。
- 主题色位于 `resources/base|dark/element/color.json`，用 `$r('app.color.*')`。
  需要深色变体的 SVG 图标放 `resources/dark/media/`。
- 路由参数必须是具名接口（`EditConnectionParams`），通过
  `this.getUIContext().getRouter()` 传递。
- ArkUI `ForEach` 的 key 必须随内容变化，否则列表项会被复用、`onClick` 闭包仍持有旧对象，
  导致 UI 与连接动作使用过期数据。本项目用 `${conn.id}#${conn.updatedAt}` 作 key。
- ArkUI `@Builder` 的参数**按值传递时不会随状态刷新**（如设置页滑块拖拽/重置后数值不更新）：
  需要响应状态变化的 builder 必须传**单个对象字面量**（按引用），并在 builder 内访问其属性。
- `JSON.parse` 反序列化：`as` 只影响编译期、不会转换成员类型，也不会恢复类默认值。本项目统一用
  `JSON.parse(text) as Record<string, Object>` 再逐字段读取（见 `ConfigTransfer.readSettings`），
  不要直接 `as` 成业务类后依赖其字段类型或默认值。

## 目录结构

| 路径 | 作用 |
|---|---|
| `entry/src/main/ets/entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置与连接存储、按默认尺寸创建主窗口、加载连接列表 |
| `entry/src/main/ets/sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸（或全屏）创建窗口、加载会话页；单窗口模式下拦截关闭以先恢复主窗口 |
| `entry/src/main/ets/pages/Index.ets` | 连接列表（点行→连接，空白区→编辑，右键菜单，右下角 FAB） |
| `entry/src/main/ets/pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏（全屏/最小化/断开）、全屏状态跟踪 |
| `entry/src/main/ets/pages/EditConnectionPage.ets` | 新增 / 编辑连接（保存仅写配置，密码连接成功后自动保存；连接按钮下方为可折叠「高级设置」） |
| `entry/src/main/ets/pages/SettingsPage.ets` | 全局设置（工具栏延迟与窗口模式开关、触控板滚动/捏合、全局分辨率/缩放、全局连接特性（剪贴板/音频/GFX/H.264/证书）、窗口默认尺寸与默认最大化、自动隐藏主窗口、底部配置导入/导出） |
| `entry/src/main/ets/services/ConnectionStore.ets` | 基于 preferences 的连接配置存储（稳定 id + updatedAt，含 `useGlobalDisplay`/`scalePercent`/`useGlobalAdvanced`） |
| `entry/src/main/ets/services/CredentialStore.ets` | 基于 ASSET 的密码存储（按连接 id，连接成功后写入） |
| `entry/src/main/ets/services/SettingsStore.ets` | 基于 preferences 的设置存储 + 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`、`SCALE_PRESETS`） |
| `entry/src/main/ets/services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；`DocumentViewPicker` + `fileIo`，密码不导出） |
| `entry/src/main/ets/services/RdpNative.ets` | 每个会话窗口一个实例（独占原生 handle）；按 handle 路由原生事件，`findByKey` 按会话 key 复用实例 |
| `entry/src/main/ets/services/SessionManager.ets` | 主窗口后台连接、每连接状态（转圈/已连接/失败）、错误分类、成功后开窗与断连编排 |
| `entry/src/main/ets/services/WindowController.ets` | 应用窗口默认尺寸、拉起独立会话窗口、会话窗口全屏与系统标题栏/dock 悬停控制、单窗口模式的主窗口隐藏/恢复 |
| `entry/src/main/cpp/hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 |
| `entry/src/main/cpp/hmrdp_session.cpp` | FreeRDP 客户端生命周期、输入、事件 |
| `entry/src/main/cpp/hmrdp_renderer.cpp` | EGL/GLES 渲染器 |
