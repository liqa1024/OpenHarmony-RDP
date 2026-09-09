# AGENTS.md

供 AI 编码助手与贡献者阅读的工程指南。项目：**RDP 远程桌面**（代号 **HmRdp**）。

## 项目定位

面向 **鸿蒙 PC（2in1）** 的 RDP 客户端。RDP 引擎为 FreeRDP 3.10.3，从源码交叉编译到
`aarch64-linux-ohos` / `x86_64-linux-ohos`。界面为 ArkTS/ArkUI；画面通过 XComponent 上的
EGL/GLES 原生渲染；输入经 Node-API 桥接转发。会话在独立的 `SessionAbility` 主窗口中打开；
主窗口 / 会话窗口的默认尺寸按屏幕比例推导，会话分辨率/缩放默认自适应当前显示器，均可在
全局设置中调整，单个连接也可在「高级设置」里覆盖。

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

## 关键实现要点（改动前必读）

1. **GFX 管线**：`HmrdpPreConnect` 必须订阅 `ChannelConnected` / `ChannelDisconnected`
   到 `freerdp_client_OnChannelConnectedEventHandler`。否则 `gdi_graphics_pipeline_init`
   不会执行，画面永远黑屏。这是最初黑屏问题的根因。
2. **鼠标按键**：`PTR_FLAGS_MOVE` 与按键事件要**分开发送**。把 `MOVE|BUTTON1|DOWN` 合并成
   一个事件会导致远端忽略点击。按键事件不能带 MOVE 标志（与 FreeRDP 的 X11 客户端一致）。
3. **密码处理**：绝不通过命令行传密码。用 `freerdp_settings_set_string` 设置
   `FreeRDP_Password` / `FreeRDP_GatewayPassword`，避免解析器失败日志与 argv 明文副本。
4. **帧上传**：渲染器每次都上传**整帧**（`glTexSubImage2D`）。按脏区部分上传会出现可见花屏。
   除非有正确实现（PBO/EGL image），否则不要改回脏区上传。
5. **原生库命名**：FreeRDP 库的 SONAME 是 `libX.so.3`；`entry/libs/<abi>/` 下必须同时有
   `libX.so`（链接期）与 `libX.so.3`（运行期）。
6. **连接与密码存储**：连接配置（host/port/用户名/选项）存 `ConnectionStore`（preferences，
   稳定 UUID 作 `id`，`updatedAt` 供列表刷新）；密码单独存 `CredentialStore`（ASSET，按 `id` 索引）。
   密码**只在连接成功后**写入（`RdpEvent.Connected` 且会话请求 `origin === Editor`）；
   从列表连接、或连接失败/取消都不写密码。清空密码只删 ASSET 密码、不删连接。
   注意 preferences 的值必须是 XML 合法字符：任意字符串字段先 `encodeURIComponent` 再用 `|`
   拼接，否则控制字符（如 `\u0001`）会让 preferences 文件损坏成 `.broken`、数据无法落盘。
7. **XComponent 输入**：surface 走 `surfaceId` +
   `OH_NativeWindow_CreateNativeWindowFromSurfaceId`（不带 `libraryname`），因此输入走 ArkUI 的
   `onMouse/onTouch/onKeyEvent/onAxisEvent`（触屏经 `onTouch` 转 RDPEI 原生触屏，见第 10 条）。
8. **窗口尺寸**：默认尺寸由 `SettingsStore.defaults()` 按屏幕宽高 × 45%（主窗口）/ 67%（会话窗口）
   计算；屏幕查询失败时**不改窗口**，而不是回退到编造的分辨率。设置项是**默认尺寸**，只在
   **窗口创建时**生效（主窗口下次启动、会话窗口下次连接）；设置页改动**不**即时 resize / 移动
   当前窗口。`window.resize()` / `moveWindowTo()` 在 2in1 上的单位是 **px**（不是 vp）。
   全局设置里可勾选「默认最大化（全屏）」（`AppSettings.sessionFullscreen`）：为真时 `SessionAbility` 经
   `startAbility` 的 `StartOptions.windowMode = WINDOW_MODE_FULLSCREEN` 直接全屏打开、忽略会话窗口尺寸。
   会话窗口的**系统最大化按钮一律转成沉浸式全屏**（`WindowController.setupSessionWindow` 监听
   `windowStatusChange`，MAXIMIZE → `maximize(ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)`；
   FULL_SCREEN → `setTitleAndDockHoverShown(false, false)`），这样系统顶部标题栏/底部 dock 悬停不弹出、
   不会挡住应用自己的顶部工具栏。全屏下由 `SessionPage` 工具栏的「全屏/退出全屏」「最小化」「断开」按钮
   代替系统标题栏（`win.maximize(ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)` / `win.minimize()` /
   `win.recover()`）。工具栏在**窗口模式下默认不显示**（`AppSettings.toolbarInWindowed` 控制），
   全屏模式下**始终显示**。
9. **会话窗口（支持多会话并存）**：用独立 `SessionAbility`（`launchType: "specified"`，
   `startAbility` 带唯一 `instanceKey`）打开，才有完整的最小化 / 最大化 / 关闭标题栏；
   `createSubWindow` 的子窗没有最小化按钮，`setWindowTitleButtonVisible` 对子窗报 1300004。
   - 每次连接生成唯一 `sessionKey`：启动页把 `PendingConnection`（含密码）放进
     `SessionRequests` 注册表，Want 只带 `sessionKey`/`instanceKey`（密码不进 Want）；
     `SessionAbility` 用 `LocalStorage` 把 key 注入页面，页面按 key 取回请求。
   - 每个会话窗口独占一个 `RdpNative` 实例 / 原生 handle：原生事件携带 handle，ArkTS 侧
     按 handle 路由到对应实例；surface 绑定/销毁、鼠标键盘输入都按 handle 下发。
   - **窗口关闭即断连**：`SessionAbility.onWindowStageDestroy()` / `onDestroy()` 调用
     `RdpNative.stopByKey(sessionKey)` + `SessionRequests.discard`，只停自己这条会话（等价于
     工具栏「断开」）。注意 `SessionPage.aboutToDisappear()` 在窗口关闭时**不会触发**，不要
     依赖它做断连。
   - `origin` 区分入口：仅编辑页发起（`Editor`）且连接成功时保存密码、并经 `onConnected`
     回调让编辑页回退到列表；列表发起（`List`）成功/失败都不做额外操作。
10. **输入分流 / 触屏 / 触控板**：鼠标走 `onMouse` → RDP 鼠标；真触屏走 `onTouch` → RDPEI
    原生触屏（`Session::SendTouch` → `freerdp_client_handle_touch`，连接时打开
    `FreeRDP_MultiTouchInput`）；滚轮 / 触控板走 `onAxisEvent`。用
    `event.source === SourceType.TouchScreen` 区分真触屏与系统把鼠标左键/轴事件转成的触摸，
    否则鼠标点击会同时被 `handleTouch` 当成触屏（或反过来模拟鼠标）导致点击变拖动。
    - FreeRDP 的 `freerdp_client_handle_touch` 用 `id == 0` 表示"空槽位"，因此 ArkUI 的
      0 基手指 id 要 `+1` 再传给原生；`handleTouch` 还需维护接触点状态（Down→Move*→Up 合法、
      未知接触点丢弃、`Cancel` 抬起），否则事件会被 RDPEI 丢弃、表现为断断续续。
    - 触控板双指滚动的轴值单位是**位移像素**、方向与鼠标滚轮**相反**（自然滚动），需累积到阈值
      再发一格；横向滚动映射为 `HWHEEL`。鼠标滚轮仍是"向前/上 = `axisVertical` 负"。
    - RDP 没有"捏合"输入 PDU：双指捏合映射为 **Ctrl + 滚轮**，整个手势期间按住 Ctrl 不松开
      （中途逐格开关会把缩放混成滚动）；灵敏度在设置里调。
    - 不要尝试用 `easy_go.json` 的 `mouse2TouchEventMode` 关闭鼠标转触摸：本机 SDK（API 26）的
      easy_go schema 不含该字段，hvigor 校验会直接失败；用上面的 source 分流替代。
11. **分辨率与缩放**：默认（全局自动）会话分辨率取当前显示器 `display.width/height`，缩放比例
    取 `densityPixels × 100`（HarmonyOS 以 160 DPI = 100%），经 `RdpOptions.scalePercent` 传给原生，
    原生只写 `FreeRDP_DesktopScaleFactor`（不碰 device scale factor）。
    自动缩放只取 Windows 固定档位 **100/125/150/175/200/225**（`SettingsStore.SCALE_PRESETS`，
    推荐值按最近档位吸附）；手动模式允许自定义任意 100–500 的值，但非档位值会在 UI 给出
    「可能导致应用模糊」的警告。全局设置页可改为手动分辨率 / 手动缩放；每个连接的高级设置里
    可开关「使用全局显示设置」，关闭后该连接用自己保存的 `width/height/scalePercent`
    （`SavedConnection.useGlobalDisplay`）。
    连接记录序列化新增 `useGlobalDisplay`、`scalePercent` 两个尾字段，旧记录（19 字段）仍可读、
    默认套用全局。连接前用 `SettingsStore.resolveDisplay(conn)` 得到最终 `DisplayProfile`。

## ArkTS 规范

- 严格 ArkTS：禁用 `any`/`unknown`，除受支持形式外禁用 `as`，禁用结构化类型、解构、
  未标注类型的对象字面量；async 必须显式标注 `Promise<T>`。
- 主题色位于 `resources/base|dark/element/color.json`，用 `$r('app.color.*')`。
  需要深色变体的 SVG 图标放 `resources/dark/media/`。
- 路由参数必须是具名接口（`EditConnectionParams`），通过
  `this.getUIContext().getRouter()` 传递。
- ArkUI `ForEach` 的 key 必须随内容变化，否则列表项会被复用、`onClick` 闭包仍持有旧对象，
  导致 UI 与连接动作使用过期数据。本项目用 `${conn.id}#${conn.updatedAt}` 作 key。

## 目录结构

| 路径 | 作用 |
|---|---|
| `entry/src/main/ets/entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置与连接存储、按默认尺寸创建主窗口、加载连接列表 |
| `entry/src/main/ets/sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸（或全屏）创建窗口、加载会话页 |
| `entry/src/main/ets/pages/Index.ets` | 连接列表（点行→连接，空白区→编辑，右键菜单，右下角 FAB） |
| `entry/src/main/ets/pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏（全屏/最小化/断开）、全屏状态跟踪 |
| `entry/src/main/ets/pages/EditConnectionPage.ets` | 新增 / 编辑连接（保存仅写配置，密码连接成功后自动保存；连接按钮下方为可折叠「高级设置」） |
| `entry/src/main/ets/pages/SettingsPage.ets` | 全局设置（工具栏延迟与窗口模式开关、触控板滚动/捏合、全局分辨率/缩放、窗口默认尺寸与默认最大化） |
| `entry/src/main/ets/services/ConnectionStore.ets` | 基于 preferences 的连接配置存储（稳定 id + updatedAt，含 `useGlobalDisplay`/`scalePercent`） |
| `entry/src/main/ets/services/CredentialStore.ets` | 基于 ASSET 的密码存储（按连接 id，连接成功后写入） |
| `entry/src/main/ets/services/SettingsStore.ets` | 基于 preferences 的设置存储 + 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`、`SCALE_PRESETS`） |
| `entry/src/main/ets/services/RdpNative.ets` | 每个会话窗口一个实例（独占原生 handle）；按 handle 路由原生事件，`stopByKey` 按会话 key 断连 |
| `entry/src/main/ets/services/WindowController.ets` | 应用窗口默认尺寸、拉起独立会话窗口、会话窗口全屏与系统标题栏/dock 悬停控制 |
| `entry/src/main/cpp/hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 |
| `entry/src/main/cpp/hmrdp_session.cpp` | FreeRDP 客户端生命周期、输入、事件 |
| `entry/src/main/cpp/hmrdp_renderer.cpp` | EGL/GLES 渲染器 |
