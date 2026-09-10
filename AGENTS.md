# AGENTS.md

供 AI 编码助手与贡献者阅读的工程指南。项目：**RDP 远程桌面**（代号 **HmRdp**）。

## AI 助手约定（重要）

- **总结必须始终使用中文**：任务收尾 / 最终答复的语言固定为中文（代码、命令、标识符除外）。
- **禁止连接用户实机调试**：出于安全考虑，不要对用户连接的**真机**执行安装、启动、调试、
  日志抓取或任何 `hdc` / `start_app` 操作；需要测试时优先采用模拟器
  （见「模拟器（功能测试）」）。
- **git 只读**：只允许用 git **查看/读取**（如 `log`、`show`、`diff`、`status`、`blame`）。
  **禁止任何写操作或借助 git 改动仓库**，包括但不限于 `commit`、`amend`、`add`、`stash`、
  `checkout`、`reset`、`revert`、`restore`、`clean`、`cherry-pick`、`rebase`、`merge`、
  `branch`、`tag`、`push`、`fetch`、`pull`。需要恢复/回退文件时，直接编辑文件内容，不要用
  git 命令改工作区或历史。

## 项目定位

面向 **鸿蒙 PC（2in1）** 的 RDP 客户端。RDP 引擎为 FreeRDP 3.10.3，从源码交叉编译到
`aarch64-linux-ohos` / `x86_64-linux-ohos`。界面为 ArkTS/ArkUI；画面通过 XComponent 上的
EGL/GLES 原生渲染；输入经 Node-API 桥接转发。会话在独立的 `SessionAbility` 主窗口中打开；
主窗口 / 会话窗口的默认尺寸按屏幕比例推导，会话分辨率/缩放默认自适应当前显示器，均可在
全局设置中调整，单个连接也可在「高级设置」里覆盖。全局设置还可开启「自动隐藏主窗口」
（单窗口模式，仅单会话：会话窗口打开时销毁主窗口，关闭后恢复，见第 14 条）。

- 应用名：**RDP 远程桌面** · Bundle：`com.lixa.hmrdp` · 目标：HarmonyOS 6.1.0（API 23）

## 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 位于 `sdk/default/openharmony`）。
- 应用 `compatibleSdkVersion` / `targetSdkVersion`：`6.1.0(23)` —— 必须与目标设备/模拟器
  （HarmonyOS 6.1.0）一致，未经确认不要擅自调高。
- ABI：`arm64-v8a`（真机，默认产物）、`x86_64`（模拟器，`emulator` target）。
- 宿主为 Windows；OpenSSL 在 WSL 中编译，驱动 Windows 版 OHOS NDK 的 `clang.exe`。

## 构建 / 运行 / 验证

产物按 **product** 分为两个变体，**DevEco Studio 右上角 `Product` 下拉即可切换**（命令行用
`--product`）：`default` 只出 arm64-v8a HAP（实机侧载用），`emulator` 只出 x86_64 HAP
（模拟器调试）：

```bash
devecocli build                                   # 仅编译（product=default → entry@default，arm64-v8a）
devecocli build --product emulator                # 仅编译模拟器包（product=emulator → entry@emulator，x86_64）
devecocli run --device "<真机序列号>"             # 编译 + 签名 + 安装 + 启动（arm）
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"   # 模拟器
```

- product → target 的映射在工程级 `build-profile.json5`：`app.products` 定义
  `default`/`emulator`，`modules[].targets[].applyToProducts` 把 `entry@default` 挂到
  `default`、`entry@emulator` 挂到 `emulator`。DevEco 切换右上角 Product 即切换 ABI。
- `devecocli run` 目前需显式带 target（`--module entry@emulator`），否则会按模块的 `default`
  target 去找产物而报 `Build metadata not found`。
- 两个 target 的 ABI/库过滤配置在 `entry/build-profile.json5`：`targets[].config.buildOption`
  覆盖 `externalNativeOptions.abiFilters` 与 `nativeLib.filter.excludes`（互斥排除另一 ABI 的
  `libs/<abi>` 目录）。`abiFilters` 只影响 CMake 编译的 ABI，**不会**过滤预置的
  `entry/libs/<abi>/*.so`，所以必须用 `nativeLib.filter.excludes` 排除。
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
native/scripts/patch-freerdp.ps1    # FreeRDP 的 OHOS 补丁（musl pthread_cancel、rdpsnd OHAudio sink、client-common SHARED、无版本号 SONAME、RDPEI 帧间隔可调）
native/scripts/build-openssl-wsl.sh # OpenSSL，在 WSL 中运行，驱动 Windows OHOS clang
native/scripts/build-freerdp.ps1    # FreeRDP 的 CMake 构建（Windows NDK）
```

`native/third_party/`、`native/build/`、`native/install/`、`native/tools/` 已 gitignore。
产出库只提交**不带版本号的单一 `libX.so`**（如 `libfreerdp3.so`）。FreeRDP 通过
`-DWITH_LIBRARY_VERSIONING=OFF`（见 `build-freerdp.ps1`）关闭库版本化，配合
`native/patches/AddTargetWithResourceFile.cmake`（见 `patch-freerdp.ps1` 第 5 步）在非 Windows
保留 `lib` 前缀与 `<名><主版本>` 输出名、并显式写入 SONAME，从而不再产生 `libX.so.3` 副本
（Linux 上 `.so` 只是指向 `.so.3` 的符号链接，Windows 上会被实体化成重复副本）。CMake 直接用
完整路径链接 `${FREERDP_LIBS}/libX.so`（见 `entry/src/main/cpp/CMakeLists.txt`），运行时
`DT_NEEDED` 也是 `libX.so`。

**优化等级 / 调试信息**：`libhmrdp.so` 由 hvigor 按构建模式重编，优化等级交给 OHOS 工具链按
`CMAKE_BUILD_TYPE` 决定（debug → `-O0 -g -fno-limit-debug-info`，release → `-O2 -DNDEBUG`）；
**不要在 `CMakeLists.txt` 里写死 `-O2`**，否则会覆盖 debug 的 `-O0`。FreeRDP 预编译库固定
`-O2 -DNDEBUG`（`build-freerdp.ps1` 写死 Release），**不跟构建模式走**。打包时 hvigor 的
`DoNativeStrip` 会 strip 所有 `.so`，HAP 内的库不含调试信息。

> **必须保持 `-DWITH_VERBOSE_WINPR_ASSERT=OFF`**（见 `build-freerdp.ps1`）：默认 ON 时
> `WINPR_ASSERT` 会 `abort()` 整个进程，任何后端健全性检查失败（如音频设备打不开）都会闪退；
> 关闭后退化为被 `NDEBUG` 禁用的 `assert()`，错误走正常分支降级。
>
> **音频：FreeRDP 只解码，播放由 `libhmrdp` 用原生 OHAudio 完成**。`native/patches/rdpsnd_opensles.c`
> （`patch-freerdp.ps1` 第 4 步）替换上游 OpenSLES 后端，把 16-bit PCM 交给 `HmrdpSetAudioSink`
> 注册的 sink（`hmrdp_session.cpp` 注册）；`hmrdp_audio.cpp` 用 `dlopen("libohaudio.so")` +
> `dlsym` 解析 OHAudio API 建 `OH_AudioRenderer`（`writeData` 拉模型 + 256KB 环形缓冲丢帧）。
> 不用 ArkTS `@ohos.multimedia.audio` 的原因：该工具链下它会触发 syscap 误报，且 OpenSLES 已废弃。
> `opensl_io.{c,h}` 仍在 opensles 的 CMake 源列表里（需第 3 步替换标识符才能编过）但已无逻辑。
> - **绝不持锁调用 Start/Stop/Release**：`OH_AudioRenderer_Release` 会等待 write 回调
>   （`JoinCallbackLoop`），而回调要同一把 `mutex_` 才能取环形缓冲，持锁 Release 必死锁
>   （表现为关闭会话后 `APP_INPUT_BLOCK` 卡死）。所以 `mutex_` 只保护环形缓冲/句柄、不跨
>   OHAudio 调用持有；`lifecycle_` 只在 `mutex_` 之外串行化 open/close；回调只碰 `mutex_`。
> - **设备能力必须优雅降级**：`AudioOutput::Supported()` 用 `dlopen` 探测（**不要**把
>   `libohaudio.so` 链成 `DT_NEEDED`，缺库设备会加载即崩）；不支持时 `Session::Connect`
>   关掉 `FreeRDP_AudioPlayback`，ArkTS 侧 `services/DeviceCapabilities.ets` 把
>   `isAudioSupported()` 暴露给 UI，设置/编辑页的「音频重定向」置灰并显示原因。
>   新增设备相关特性沿用此 `Capability` 模式（置灰 + 原因 + 原生兜底），别只靠 syscap 告警。
> 改动 FreeRDP 侧后需重编并提交 `entry/libs/<abi>/*.so`；只改应用层不用重编。

## 关键实现要点（改动前必读）

1. **GFX 管线**：`HmrdpPreConnect` 必须把 `ChannelConnected`/`ChannelDisconnected` 订阅到
   `freerdp_client_OnChannelConnectedEventHandler`，否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
2. **鼠标按键**：`PTR_FLAGS_MOVE` 与按键事件分开送，按键事件不带 MOVE 标志，否则远端忽略点击。
3. **密码**：绝不经命令行传密码，用 `freerdp_settings_set_string` 设 `FreeRDP_Password` /
   `FreeRDP_GatewayPassword`。
4. **帧上传**：每次上传整帧（`glTexSubImage2D`）；按脏区部分上传会花屏，除非改用 PBO/EGL image。
5. **原生库命名**：只提交不带版本号的单一 `entry/libs/<abi>/libX.so`（FreeRDP 以
   `WITH_LIBRARY_VERSIONING=OFF` 构建，SONAME 也是 `libX.so`）；CMake 用完整路径链接它，
   运行期 `DT_NEEDED` 也是 `libX.so`（不再产生 `libX.so.3` 副本）。
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
    id 要 `+1`（FreeRDP 用 `id == 0` 表示空槽）。触屏还有两条硬性约束：①`handleMouse` 过滤
    `source===TouchScreen` 的兼容鼠标事件，避免一次触摸走两条通路产生杂点击；②重复 `TouchType.Down`
    **忽略**而不是补发 UP+DOWN，否则远端会看到"松开+重按"而冒出单击。接触压力按
    `RdpTouchFlags.HAS_PRESSURE` 透传（ArkUI `[0,65535)` → RDPEI `[0,1024]`，0 表示设备未上报）。
    FreeRDP 侧 `rdpei` 每 **20ms** 才发一帧且同一接触点会被覆盖合并，故触屏实际上限约 50fps（这是
    RDPEI 触屏与 mstsc 的主要差距）。`patch-freerdp.ps1` 第 6 步把该间隔改成运行时全局
    `HmrdpSetTouchFrameInterval`；`libhmrdp` 以**弱符号**引用它，全局设置「触屏-高刷新率」开启时传 0
    （逐帧下发，默认关闭传 20 保持上游行为）。未打补丁的 FreeRDP 上弱符号为空、开关自动降级为无效。
    改动后需重编 FreeRDP 并回写 `entry/libs/<abi>/*.so`。触控板双指滑动靠
    `event.sourceTool === SourceTool.TOUCHPAD` 区分（其 `axisVertical/axisHorizontal` 是**本次事件的 vp
    位移**而非轮齿，`sourceType` 为 Unknown）：`services/TouchpadWheel.ets` 的 `TouchpadWheelMapper`
    按 `120 / 16vp × 速度倍率` 把位移累加成**高分辨率 RDP 轮转量**（9bit 二补码、120=1 齿、单事件
    上限 0xFF 自动分片，与 FreeRDP SDL 客户端对齐），横向映射 `HWHEEL`；全局设置「滚动速度」
    `touchpadScrollSpeed`（倍率，默认 1.0×，0.2×~4×）只缩放轮转量、不改变发射粒度。捏合默认
    「缩放速度」`pinchZoomSpeed`（倍率，默认 1.0×，越大越快）线性映射为 Ctrl+滚轮，每
    `1/(20×speed)` 的捏合比例变化发一格；全局设置「使用触摸模拟触控板捏合」（`pinchAsTouch`，默认关）
    开启后改为在鼠标位置合成**两个原生触点**（外部 id 20/21）做真实双指缩放：触点距离按 `axisPinch`
    **1:1** 映射（`缩放速度` 只用于 Ctrl+滚轮模式，触摸模式下**置灰**），初始间距由全局设置「触点初始距离」
    `pinchTouchDistance`（默认 4%，取窗口较短边的百分比，**不写死分辨率**）决定。慢速捏合会被系统拆成多段
    `BEGIN/END`，故 `AxisAction.END` 不立即抬指，而是用 `PINCH_END_DEBOUNCE_MS`(150ms) 去抖、期间新段沿用
    同一对触点（否则每段都"松开+重按"产生假点击），`PINCH_SAFETY_MS` 兜底。注意 ArkUI
    `AxisType` 只有 `VERTICAL/HORIZONTAL/PINCH`，**拿不到捏合的手指朝向/旋转**（官方明确触控板多指不上报
    手指信息、RotationGesture 也不支持触控板旋转），故模拟触点按全局设置「触摸捏合角度」
    `pinchTouchAngle`（度，正=顺时针=左手，负=右手，默认 30°；-30° 即右手）斜置。别用 `easy_go.json` 的 `mouse2TouchEventMode`
    关鼠标转触摸（本机 SDK schema 不含该字段，hvigor 校验失败）。
11. **分辨率与缩放**：自动分辨率取显示器宽高，缩放取 `densityPixels × 100` 并吸附到
    100/125/150/175/200/225（`SettingsStore.SCALE_PRESETS`）；经 `RdpOptions.scalePercent` → 原生只写
    `FreeRDP_DesktopScaleFactor`。每个连接可关「使用全局显示设置」用自己保存的
    `width/height/scalePercent`；连接前用 `SettingsStore.resolveDisplay(conn)` 解析。
12. **高级连接特性（全局默认 + 单连接覆盖）**：音频/GFX/H.264/忽略证书这 4 项默认值放在
    `AppSettings`（设置页「连接特性」区），连接保存自己的独立值 + `useGlobalAdvanced` 标志，
    **默认跟随全局**（`SavedConnection.useGlobalAdvanced = true`）。连接前用
    `SettingsStore.resolveAdvanced(conn)` 解析，再写入 `RdpConnectOptions`。编辑页「高级设置」里
    「使用全局高级设置」关掉后才会用本连接的独立开关；保存时仍持久化独立值，便于随时切回。
    剪贴板固定开启（其全局开关已移除），但仍保留单连接的「剪贴板重定向」开关可单独关闭。
13. **剪贴板重定向（文本，手动触发、无权限）**：原生在 `HmrdpChannelConnected` 里先调用默认
    handler 再捕获 `CliprdrClientContext`，只覆写 `Server*` 回调与 `MonitorReady`，**不要动
    `Client*` 发送函数**；文本按 CF_UNICODETEXT 传输，本地缓存为 NUL 结尾 UTF-16LE。
    - 会话工具栏里「复制」「粘贴」两个按钮在窗口动作左侧（蓝调底色 + `bindTips` 说明），连接
      关闭「剪贴板重定向」时不显示。语义：复制＝远端→本机，粘贴＝本机→远端。
    - 粘贴用 `PasteButton` 安全控件（系统固定文字 + `PasteIconStyle.LINES`）授权后 `getData()`
      读本机文本 → `native.setClipboard(text)`，**不申请任何剪贴板权限**（安全控件文字/图标/对比
      受约束，字号过小会导致授权失败）。
    - 复制由原生在收到 `FORMAT_LIST` 时主动请求 CF_UNICODETEXT 并 `Emit(ClipboardText)`，
      `SessionPage` 仅缓存到 `remoteClipboardText`；点「复制」才 `setData` 写本机（无需权限）。
      **不自动写本机剪贴板、不监听 `update`**（原 `ClipboardSync` 已删）。仅支持纯文本。
14. **自动隐藏主窗口（单窗口模式）**（`AppSettings.autoHideMainWindow`，默认关）：开启后仍**新建**
    `SessionAbility` 会话窗，但**销毁主 `EntryAbility`** 以真正隐藏（无 hide API，`minimize()` 仍在
    Dock）；按单会话设计，故 `WindowController` 只用 `mainHidden` 布尔量，不跟踪 session 集合。
    - **进程内最后一个 UIAbility 被销毁 → 进程退出**：必须等会话窗加载完成（`onSessionWindowReady`）
      后才 `terminateSelf()` 主窗口，否则 `Terminate last` 会杀掉整个应用。
    - 关闭会话时先 `startAbility(EntryAbility)` 拉起主窗口，`onMainWindowReady` 后再终止会话；用
      `windowStage.on('windowStageClose')` + `UIAbility.onPrepareToTerminate()` 拦截关闭（本机模拟器
      后者不触发），`closeSession` 去重 + 3s 超时兜底。关闭该选项则行为不变（多窗口可并存）。
15. **配置导入/导出**：`ConfigTransfer` 把全局设置 + 全部连接导出为 JSON（`picker.DocumentViewPicker`
    选择文件/路径，`fileIo` 读写）。密码在 ASSET 中，**不导出**，UI 与弹窗需提示「导入后需重新输入」。
    导出用独立的 `ExportedConnection` DTO 而非直接序列化 `SavedConnection`，否则会带出继承的
    `password`/`gatewayPassword` 字段；导入按连接 `id` 覆盖/新增并刷新 `updatedAt`。入口在设置页底部。
16. **自定义 Win 键映射**（全局设置 `winKeySubstitute`，0＝关闭；设置页点按按键框后按下任意键自动识别并显示
    名称，可清除；名称表在 `KeyMapper.describe`）：实现是在 `SessionPage.handleKey` 里，若
    `event.keyCode === winKeySubstitute` 就改发 Meta（`RdpNative.winKey` → `sendKey(0x5B, ext)`）。
    **真实 Win（2076/2077）不转发**，避免"远端+本机"双重映射。
    历史：窗口级「按键穿透」与 `keyboardShortcutPassthrough` 开关已删除（对系统保留键只能旁听、
    拦不住 shell，对普通键又无必要）；确需独占系统快捷键只能用系统级
    `OH_Input_AddKeyEventInterceptor`（`system_basic`）或 `OH_Input_AddKeyEventHook`。
17. **会话工具栏显示**：全屏模式沿用悬浮自动隐藏（`toolbarHoverDelay`/`toolbarHideDelay`，鼠标靠近屏幕顶部
    才弹出，`updateToolbar` 只在全屏生效）；窗口模式**始终显示**，且作为普通行布局在远程画面**上方**
    （`Column`：工具栏 + `Stack`(XComponent + 状态浮层)），渲染区域自然扣除工具栏高度、不再被覆盖，
    指针映射仍以 XComponent 局部坐标为准。

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
| `entry/src/main/ets/pages/Index.ets` | 连接列表（点左侧→编辑，右侧圆钮→连接，右键菜单，拖拽排序，右下角 FAB） |
| `entry/src/main/ets/pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏（右侧依次复制/粘贴、全屏/最小化/断开）、全屏状态跟踪 |
| `entry/src/main/ets/pages/EditConnectionPage.ets` | 新增 / 编辑连接（保存仅写配置，密码连接成功后自动保存；连接按钮下方为可折叠「高级设置」） |
| `entry/src/main/ets/pages/SettingsPage.ets` | 全局设置（工具栏延迟、触控板滚动速度/捏合、Win 键替代、全局分辨率/缩放、全局连接特性（音频/GFX/H.264/证书）、窗口默认尺寸与默认最大化、自动隐藏主窗口、底部配置导入/导出） |
| `entry/src/main/ets/services/ConnectionStore.ets` | 基于 preferences 的连接配置存储（稳定 id + updatedAt，含 `useGlobalDisplay`/`scalePercent`/`useGlobalAdvanced`；`reorder` 持久化拖拽后的 `ids` 顺序） |
| `entry/src/main/ets/services/CredentialStore.ets` | 基于 ASSET 的密码存储（按连接 id，连接成功后写入） |
| `entry/src/main/ets/services/SettingsStore.ets` | 基于 preferences 的设置存储 + 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`、`SCALE_PRESETS`） |
| `entry/src/main/ets/services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；`DocumentViewPicker` + `fileIo`，密码不导出） |
| `entry/src/main/ets/services/RdpNative.ets` | 每个会话窗口一个实例（独占原生 handle）；按 handle 路由原生事件，`findByKey` 按会话 key 复用实例 |
| `entry/src/main/ets/services/TouchpadWheel.ets` | 触控板 vp 位移 → 高分辨率 RDP 轮转量映射（`TouchpadWheelMapper`，速度倍率、9bit 二补码、0xFF 分片） |
| `entry/src/main/ets/services/DeviceCapabilities.ets` | 运行时设备能力探测（`Capability{supported,reason}`）；音频能力查 `isAudioSupported()`，供设置/编辑页置灰并给出原因 |
| `entry/src/main/ets/services/SessionManager.ets` | 主窗口后台连接、每连接状态（转圈/已连接/失败）、错误分类、成功后开窗与断连编排 |
| `entry/src/main/ets/services/WindowController.ets` | 应用窗口默认尺寸、拉起独立会话窗口、会话窗口全屏与系统标题栏/dock 悬停控制、单窗口模式的主窗口隐藏/恢复 |
| `entry/src/main/cpp/hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 + `isAudioSupported` 能力查询 |
| `entry/src/main/cpp/hmrdp_session.cpp` | FreeRDP 客户端生命周期、输入、事件 |
| `entry/src/main/cpp/hmrdp_renderer.cpp` | EGL/GLES 渲染器 |
| `entry/src/main/cpp/hmrdp_audio.cpp` | `dlopen` OHAudio 的 PCM 播放器（能力探测 + 环形缓冲 + 中断/错误降级） |
