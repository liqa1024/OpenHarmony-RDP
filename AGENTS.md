# AGENTS.md

供 AI 编码助手与贡献者阅读的工程指南。项目：**RDP 远程桌面**（代号 **HmRdp**）。

## 项目定位

面向 **鸿蒙 PC（2in1）** 的 RDP 客户端。RDP 引擎为 FreeRDP 3.10.3，从源码交叉编译到
`aarch64-linux-ohos` / `x86_64-linux-ohos`。界面为 ArkTS/ArkUI；画面通过 XComponent 上的
EGL/GLES 原生渲染；输入经 Node-API 桥接转发。会话在独立的 `SessionAbility` 主窗口中打开；
主窗口 / 会话窗口的默认尺寸按屏幕比例推导，可在全局设置中调整。

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
6. **凭证**：经 `@kit.AssetStoreKit` 存储。要列出全部，先用 `ReturnType.ATTRIBUTES` 查别名，
   再逐条 `loadConnection(alias)`；直接查 `ReturnType.ALL` 会返回空。
7. **XComponent 输入**：surface 走 `surfaceId` +
   `OH_NativeWindow_CreateNativeWindowFromSurfaceId`（不带 `libraryname`），因此输入走 ArkUI 的
   `onMouse/onTouch/onKeyEvent/onAxisEvent`。
8. **窗口尺寸**：默认尺寸由 `SettingsStore.defaults()` 按屏幕宽高 × 45%（主窗口）/ 67%（会话窗口）
   计算；屏幕查询失败时**不改窗口**，而不是回退到编造的分辨率。设置项是**默认尺寸**，只在
   **窗口创建时**生效（主窗口下次启动、会话窗口下次连接）；设置页改动**不**即时 resize / 移动
   当前窗口。`window.resize()` / `moveWindowTo()` 在 2in1 上的单位是 **px**（不是 vp）。
9. **会话窗口**：用独立 `SessionAbility`（`launchType: "specified"`，`startAbility` 带唯一
   `instanceKey`）打开，才有完整的最小化 / 最大化 / 关闭标题栏；`createSubWindow` 的子窗没有
   最小化按钮，`setWindowTitleButtonVisible` 对子窗报 1300004。连接参数经 `PendingConnection`
   静态传递（同一进程共享 ArkTS VM），密码不放进 Want。

## ArkTS 规范

- 严格 ArkTS：禁用 `any`/`unknown`，除受支持形式外禁用 `as`，禁用结构化类型、解构、
  未标注类型的对象字面量；async 必须显式标注 `Promise<T>`。
- 主题色位于 `resources/base|dark/element/color.json`，用 `$r('app.color.*')`。
  需要深色变体的 SVG 图标放 `resources/dark/media/`。
- 路由参数必须是具名接口（`EditConnectionParams`），通过
  `this.getUIContext().getRouter()` 传递。

## 目录结构

| 路径 | 作用 |
|---|---|
| `entry/src/main/ets/entryability/EntryAbility.ets` | 主窗口 Ability：初始化设置、按默认尺寸创建主窗口、加载连接列表 |
| `entry/src/main/ets/sessionability/SessionAbility.ets` | 独立会话窗口 Ability：按默认尺寸创建窗口、加载会话页 |
| `entry/src/main/ets/pages/Index.ets` | 连接列表（点行→连接，空白区→编辑，右键菜单，右下角 FAB） |
| `entry/src/main/ets/pages/SessionPage.ets` | XComponent 画面、输入、浮层、自动隐藏工具栏 |
| `entry/src/main/ets/pages/EditConnectionPage.ets` | 新增 / 编辑连接 |
| `entry/src/main/ets/pages/SettingsPage.ets` | 全局设置（工具栏触发/隐藏延迟、窗口默认尺寸） |
| `entry/src/main/ets/services/CredentialStore.ets` | 基于 ASSET 的凭证存储 |
| `entry/src/main/ets/services/SettingsStore.ets` | 基于 preferences 的设置存储（含按屏幕比例推导的窗口默认尺寸） |
| `entry/src/main/ets/services/WindowController.ets` | 应用窗口默认尺寸、拉起独立会话窗口 |
| `entry/src/main/cpp/hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 |
| `entry/src/main/cpp/hmrdp_session.cpp` | FreeRDP 客户端生命周期、输入、事件 |
| `entry/src/main/cpp/hmrdp_renderer.cpp` | EGL/GLES 渲染器 |
