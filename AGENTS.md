# AGENTS.md

供 AI 编码助手与贡献者阅读的工程指南。项目：**RDP 远程桌面**（代号 **HmRdp**）。

## AI 助手约定（重要）

- **总结必须始终使用中文**：任务收尾 / 最终答复的语言固定为中文（代码、命令、标识符除外）。
- **Vulkan / 硬件加速例外（重要）：硬件加速（Vulkan GPU 引擎）与 GPU 回放测试是"真机专属功能"。**
  模拟器**不在支持范围内**——它的 Vulkan 实现**会按标准接口谎报能力**（host↔device 内存桥两个方向都
  失效，部分核心命令还会直接崩溃），因此**不为它写降级/适配分支，也不采信其上的任何
  结论**；Vulkan 相关工作一律在真机上进行，并由 AI 自动完成：`devecocli build`（hvigor 顺带签名）→
  `native/scripts/install-device.ps1 -Device "<序列号>"` → `hdc shell "hilog -x -D 0xD001"` 读结论。
  **仓库里不出现任何签名路径/口令**。模拟器仍用于 ArkTS/UI/逻辑/结构类调试，但**不参与 Vulkan 的
  任何验证**。注意：模拟器上会崩的命令在真机上**实测正常**（缩放上屏已在用），
  **不要把模拟器结论当成对真机的约束**。依据、最小复现与门禁见 `VULKAN-TODO.md` §3.4/§3.5。
- **实机操作默认禁止、需明确授权，且是最后手段**（**Vulkan 后端除外**：见上条，其真机操作已获授权、
  由 AI 自动执行）：日常功能/回归测试一律先用模拟器（见「模拟器（功能测试）」）；
  **不要动辄"退回真机"、更不要把结论甩给真机**——上真机很麻烦、也很危险（先前的
  重型 GPU 负载曾把整机/模拟器弄到黑屏）。仅当**已确认是只有真机能判定的 GPU/驱动差异**、且用户**明确
  同意**本次实机验证时才操作，且：**签名与安装由用户完成**（仓库不含签名材料），AI 只做 push 样本 /
  重启 / 读 `hilog` 等无破坏性动作。
- **先怀疑代码与模拟器自身状态，而不是设备**：模拟器出现异常时，先按"代码 / 构建 / 数据"方向排查；若此前
  跑过重型 GL/GPU 压测，**先冷启动模拟器再复现**（渲染后端可能被压坏，冷启动即恢复），不要据此断言
  "真机为准"或急着换设备。
- **git 只读**：只允许用 git **查看/读取**（如 `log`、`show`、`diff`、`status`、`blame`）。
  **禁止任何写操作或借助 git 改动仓库**，包括但不限于 `commit`、`amend`、`add`、`stash`、
  `checkout`、`reset`、`revert`、`restore`、`clean`、`cherry-pick`、`rebase`、`merge`、
  `branch`、`tag`、`push`、`fetch`、`pull`。需要恢复/回退文件时，直接编辑文件内容，不要用
  git 命令改工作区或历史。

## 项目定位

面向 **鸿蒙 PC（2in1）** 的 RDP 客户端。RDP 引擎为 FreeRDP 3.10.3，从源码交叉编译到
`aarch64-linux-ohos` / `x86_64-linux-ohos`。界面为 ArkTS/ArkUI；输入经 Node-API 桥接转发。
**GFX 画面默认由 GPU 桌面引擎接管**（多表面 + 合成 + 上屏，见第 20 条），设置页「硬件解码」
关闭或设备不支持时回退 FreeRDP gdi。
**注意：硬件加速正在从 GLES 迁到纯 Vulkan，且是「真机专属功能」**（模拟器不在支持范围内，
见「AI 助手约定」与 `VULKAN-TODO.md`）：切换完成后 Vulkan 引擎只按真机设计，模拟器上**不开启**硬件
加速、也不做 GPU 回放测试（相关 UI 置灰与原因是待跟进的代码工作）。
会话在独立的 `SessionAbility` 主窗口中打开；
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

**签名：配置放仓库外、由 hvigor 注入；不提交任何签名信息**（`build-profile.json5` 的
`signingConfigs` 恒为 `[]`）：

- `.signing/signing-config.json`（已 gitignore）放**本机**签名配置（DevEco 自动签名产生的那段
  `type` + `material`，口令是 DevEco 的密文，只有 hvigor 能解）；
- 工程级 `hvigorfile.ts` 通过 `config.ohos.overrides.signingConfig` 把它注入（官方《动态修改编译配置》
  方式二）。文件不存在时自动跳过，构建仍可用（只出未签名 HAP）。

```powershell
devecocli build                                              # 同时产出 entry-default-signed.hap
native/scripts/install-device.ps1 -Device "<真机序列号>"     # 安装 + 启动（不接触签名材料）
```

- product → target 的映射在工程级 `build-profile.json5`：`app.products` 定义
  `default`/`emulator`，`modules[].targets[].applyToProducts` 把 `entry@default` 挂到
  `default`、`entry@emulator` 挂到 `emulator`。DevEco 切换右上角 Product 即切换 ABI。
- `devecocli run` 目前需显式带 target（`--module entry@emulator`），否则会按模块的 `default`
  target 去找产物而报 `Build metadata not found`。
- 两个 target 的 ABI/库过滤配置在 `entry/build-profile.json5`：`targets[].config.buildOption`
  覆盖 `externalNativeOptions.abiFilters` 与 `nativeLib.filter.excludes`（互斥排除另一 ABI 的
  `libs/<abi>` 目录）。`abiFilters` 只影响 CMake 编译的 ABI，**不会**过滤本地放在
  `entry/libs/<abi>/*.so` 的库，所以必须用 `nativeLib.filter.excludes` 排除。
- 改动 `.ets` 后先跑 `arkts_check`，再跑 `build_project`。
- 任务结束前 `build_project` 必须通过。
- 仓库**不含签名材料**，`build-profile.json5` 的 `signingConfigs` **保持 `[]`**：签名配置放仓库外
  `.signing/signing-config.json`，由 `hvigorfile.ts` 注入（见上）。**DevEco 重新自动签名后**，把它写回的
  那段 `material` 覆盖到该文件，并把 `build-profile.json5` 改回 `[]`。切勿提交 `*.p12/p7b/cer`/keystore
  或任何签名路径。

### 模拟器（功能测试）

- 首选 **2in1 模拟器**（HarmonyOS 6.1.0 / x86_64），用于**与硬件加速无关**的调试。
- 启动：`devecocli emulator start "<模拟器名>"`（名字取自本机已安装的模拟器列表）。
  注意：同一时刻只有一个实例占用 5555。
- `uitest uiInput` 注入的是**触摸**事件，不会触发 `onMouse`；鼠标请用 `uinput -M ...`，
  但其 `-m` 是**相对/累加**移动且指针常"不可见"，精确定位不可靠。
- 精确坐标：`uitest dumpLayout -p /data/local/tmp/layout.json` + `hdc file recv`，按控件
  `bounds` 计算 `uitest uiInput click <x> <y>` 的中心点（比目测截图可靠）。
- 截图：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg` + `hdc file recv`。
- 连接多个设备时 `hdc` 需用 `-t <serial>` 指定目标。
- **模拟器优先，不轻易上真机**：模拟器足以判定绝大多数**功能 / 逻辑**问题。真机 GPU 与模拟器在
  驱动行为上**可能有差异**，但**只有确认属于这类设备相关差异时**才在用户明确授权下用真机复验，
  不要把它当默认步骤或万能退路。
- 排查顺序：**代码 / 构建 / 数据 → 模拟器自身状态（重型 GL 压测后先冷启动模拟器）→ 真机（需授权）**。
- **硬件加速 / GPU 回放 / Vulkan 不走这条**：它们是**「真机专属功能」**，模拟器**不在支持范围内**
  （见开头「Vulkan / 硬件加速例外」与 `VULKAN-TODO.md` §3.4）；Vulkan 相关工作一律在真机、由 AI 按
  `VULKAN-TODO.md` §3.5 的循环自动执行。模拟器只用于**与硬件加速无关**的 ArkTS / UI / 逻辑 / 结构调试。

## 原生库源码构建（**预编译库不再提交**）

```
native/scripts/patch-freerdp.ps1    # FreeRDP 的 OHOS 补丁（musl pthread_cancel、rdpsnd OHAudio sink、client-common SHARED、无版本号 SONAME、RDPEI 帧间隔可调、GFX 原始流采集/回放钩子、ClearCodec CPU 解码）
native/scripts/build-zlib.ps1       # zlib 静态库（Windows NDK）；FreeRDP 静态链入
native/scripts/build-openssl-wsl.sh # OpenSSL，在 WSL 中运行，驱动 Windows OHOS clang
native/scripts/build-freerdp.ps1    # FreeRDP 的 CMake 构建（Windows NDK）
```

`native/third_party/`、`native/build/`、`native/install/`、`native/tools/` 已 gitignore。
**`entry/libs/<abi>/` 也必须 gitignore、不得提交**：这些 `.so` 里带着**构建机的绝对安装路径**
（`CMAKE_INSTALL_PREFIX` 会被编进 winpr 等库），属于本机环境信息。**clone 后先本地构建**，
把产物放到 `entry/libs/<abi>/` 再编译应用（`entry/src/main/cpp/CMakeLists.txt` 按
`${FREERDP_LIBS}/libX.so` 完整路径链接）。

产出库是**不带版本号的单一 `libX.so`**（如 `libfreerdp3.so`）：`build-freerdp.ps1` 用
`-DWITH_LIBRARY_VERSIONING=OFF`，配合 `native/patches/AddTargetWithResourceFile.cmake`
（`patch-freerdp.ps1` 第 5 步）在非 Windows 保留 `lib` 前缀并显式写入 SONAME，避免 Windows 上
被实体化成 `libX.so.3` 重复副本；运行时 `DT_NEEDED` 同样是 `libX.so`。
同理，`thirdparty/freerdp/include/.../winpr/build-config.h` 里的 `WINPR_INSTALL_*` 保持
**中性值**（`build-freerdp.ps1` 安装后会自动归一化），别把带绝对路径的版本抄进来。

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
>
> **H.264/AVC 已移除**：曾用 OHOS AVCodec 实现了 H.264 硬解子系统（可参考 git 历史/PERF-TODO），
> 但真机验证发现：(1) H.264 只有在客户端广告 **AVC444** 时 Windows 才启用，属微软非核心可选项；
> (2) 即使命中硬解（`hardware=1`）也无明显收益——瓶颈在解码后的 CPU 环节（拷帧/YUV→RGB/合成/上传）。
> 故**整体砍掉 H.264 支持**（`WITH_GFX_H264` 保持 OFF，`Connect` 显式 `GfxH264=false`），其余码流
> （RemoteFX Progressive / ClearCodec 等）由服务端按内容选择。`patch-freerdp.ps1`/`build-freerdp.ps1`
> 不再加 H.264 子系统，`libfreerdp3.so` 也不再有媒体库 `DT_NEEDED`。全局设置保留 `硬件解码` 开关，
> 语义面向 **GPU 加速管线**（Vulkan；见 `VULKAN-TODO.md` §4.2 与第 20 条），且是**「真机专属功能」**
> （模拟器不在支持范围）。GPU 引擎正在整体改为 Vulkan；旧的 `hmrdp_rfx.cpp`（GLES）已冻结、只作参考，
> 随 V7 删除。改动 FreeRDP 侧后需重编，并把产物放到 `entry/libs/<abi>/`（该目录已 gitignore，不再提交）；
> 只改应用层不用重编。

## 关键实现要点（改动前必读）

> **全局设置生效时机**：所有设置项（分辨率/缩放、连接特性、触屏高刷新率、使用RDP光标等）在
> **连接建立时**读取，改动后需**重开会话**才生效；已打开的会话不受影响。

1. **GFX 管线**：`HmrdpPreConnect` 必须把 `ChannelConnected`/`ChannelDisconnected` 订阅到
   `freerdp_client_OnChannelConnectedEventHandler`，否则 `gdi_graphics_pipeline_init` 不执行、画面全黑。
2. **鼠标按键**：`PTR_FLAGS_MOVE` 与按键事件分开送，按键事件不带 MOVE 标志，否则远端忽略点击。
3. **密码**：绝不经命令行传密码，用 `freerdp_settings_set_string` 设 `FreeRDP_Password` /
   `FreeRDP_GatewayPassword`。
4. **帧呈现（gdi 回退路径）**：按脏区部分上传 / 呈现。GLES 版靠 `glTexSubImage2D` +
   `GL_UNPACK_ROW_LENGTH`（= 整桌面 stride/4，否则默认按上传宽读行会花屏）；Vulkan 版对应
   `vkCmdCopyBufferToImage` + `bufferRowLength`（GLES 那套随 V7 删除）。
   **不要再叠加 present-on-change**：静止态早已由 FreeRDP 失效区门控保证——`HmrdpBeginPaint`
   把 `hwnd->invalid->null` 置 TRUE，只有真正执行绘制原语时 `gdi_InvalidateRegion` 才置 FALSE，
   而 `HandleEndPaint` 对 `null` 直接 return，故静止桌面根本不进呈现路径。额外 `memcmp`
   只增内存/带宽开销（高动态大脏区时反而耗电）。
5. **原生库命名**：产物是不带版本号的单一 `libX.so`（放在**已 gitignore** 的 `entry/libs/<abi>/`，
   **不再提交**——库内带本机构建路径）。原因与 SONAME 处理见「原生库源码构建」。
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
10. **输入分流**：鼠标→`onMouse`、真触屏→`onTouch`（RDPEI）、滚轮/触控板→`onAxisEvent`。
    - **触屏**（只认 `event.source===SourceType.TouchScreen`；手指 id `+1` 避开 FreeRDP 的 0 空槽）：
      ① `handleMouse` 过滤同源兼容鼠标事件，避免一触两通路产生杂点击；② 重复 `TouchType.Down` 忽略
      （补发 UP+DOWN 会变成"松开+重按"的假点击）；③ `TouchType.Cancel` 不代表抬指（move 会被打断）：
      挂起不抬指，若附近随后出现新的 Down/Move 判定为同指续接触、只发 MOTION（`touchContactIds` 保留
      远端 contactId），`CANCEL_HOLD_MS`(400ms) 内无续接触才真正 UP。压力按 `RdpTouchFlags.HAS_PRESSURE`
      透传（`[0,65535)`→`[0,1024]`，0 表示设备未上报）。
    - **触屏高刷新率**：FreeRDP `rdpei` 默认每 20ms 才发一帧（≈50fps，与 mstsc 的主要差距）；
      `patch-freerdp.ps1` 第 6 步导出运行时全局 `HmrdpSetTouchFrameInterval`，`libhmrdp` 以弱符号引用，
      设置「触屏-高刷新率」开启传 0、默认 20。未打补丁的 FreeRDP 上弱符号为空、开关自动降级。改后需
      重编 FreeRDP 并把产物放到 `entry/libs/<abi>/`（已 gitignore）。
    - **触控板滑动**（`event.sourceTool===SourceTool.TOUCHPAD`，`axisVertical/Horizontal` 是本次事件的 vp
      位移而非轮齿）：`services/TouchpadWheel.ets` 按 `120/16vp × 速度倍率` 累加成**高分辨率 RDP 轮转量**
      （9bit 二补码、120=1 齿、单事件 ≤0xFF 分片，对齐 FreeRDP SDL），横向 `HWHEEL`；设置「滚动速度」
      `touchpadScrollSpeed`（默认 1.0×）只缩放轮转量、不改变发射粒度。
    - **触控板捏合**：默认按 `1/(20×speed)` 线性映射为 Ctrl+滚轮（「缩放速度」`pinchZoomSpeed`）；设置
      「使用触摸模拟触控板捏合」`pinchAsTouch`（默认关）改为在鼠标位置合成两个原生触点（id 20/21）做真实
      双指缩放，距离按 `axisPinch` **1:1**（速度置灰），初始间距取窗口较短边百分比 `pinchTouchDistance`
      （默认 4%，不写死分辨率），按 `pinchTouchAngle`（默认 30°；-30° 为右手）斜置。**不设计时器**，只在
      真实 `AxisAction.END` 结束，静止保持时触点/Ctrl 一直按住。**起手 30ms（`PINCH_TOUCH_DOWN_FLUSH_MS`）
      内不发 MOTION**：避免 FreeRDP 的 RDPEI 50Hz 合帧把未发出的 `DOWN` 覆盖成 `UPDATE`（关掉高刷时捏合
      失效）；该时长远低于 Windows 的捏合判定所需时间，不影响识别。ArkUI `AxisType` 无旋转轴，拿不到真实
      手指朝向（触控板多指不上报手指信息、RotationGesture 不支持触控板旋转），故用角度设置顶替。
    - 别用 `easy_go.json` 的 `mouse2TouchEventMode`（本机 SDK schema 不含该字段，hvigor 校验失败）。
11. **分辨率与缩放**：自动分辨率取显示器宽高，缩放取 `densityPixels × 100` 并吸附到
    100/125/150/175/200/225（`SettingsStore.SCALE_PRESETS`）；经 `RdpOptions.scalePercent` → 原生只写
    `FreeRDP_DesktopScaleFactor`。每个连接可关「使用全局显示设置」用自己保存的
    `width/height/scalePercent`；连接前用 `SettingsStore.resolveDisplay(conn)` 解析。
12. **高级连接特性（全局默认 + 单连接覆盖）**：音频/忽略证书这几项默认值放在
    `AppSettings`（设置页「连接特性」区），连接保存自己的独立值 + `useGlobalAdvanced` 标志，
    **默认跟随全局**（`SavedConnection.useGlobalAdvanced = true`）。连接前用
    `SettingsStore.resolveAdvanced(conn)` 解析，再写入 `RdpConnectOptions`。编辑页「高级设置」里
    「使用全局高级设置」关掉后才会用本连接的独立开关；保存时仍持久化独立值，便于随时切回。
    剪贴板固定开启（其全局开关已移除），但仍保留单连接的「剪贴板重定向」开关可单独关闭。
    **GFX 恒开**：关掉图形管线会让会话完全不可用，故其开关已删除，`Connect` 里无条件置
    `FreeRDP_SupportGraphicsPipeline`（`AppSettings.enableGfx` 字段保留但已不生效）。
13. **剪贴板重定向（手动触发）**：**同步在设计上就是用户手动触发的**——工具栏「复制」（远端→
    本机）与「粘贴」（本机→远端）两个按钮，**刻意不做自动同步**，扩展图片/文件等类型时也必须沿用。
    理由：①自动读本机剪贴板需要受限开放的 `READ_PASTEBOARD` 权限、有隐私成本，手动则可用
    `PasteButton` 安全控件临时授权、无需声明任何权限；②图片/文件等大数据量只有显式触发才可控
    （二次确认、进度、目标路径）；③两个方向逻辑对称一致（都是「读一侧 → 写另一侧」），扩展类型
    只需改中间那段转换。**不要引入 `on('update')` 自动监听或自动写本机剪贴板**（原 `ClipboardSync`
    已删）。实现上原生只覆写 `Server*` 回调与 `MonitorReady`，不动 `Client*` 发送函数。
    当前支持纯文本 / 富文本（HTML）/ 图片；文件传输预留（待 UI 设计后再做）。两个按钮按实际类型弹
    差异化提示（文本/富文本/图片）。
    - **远端格式协商**：`FORMAT_LIST` 按 `图片(DIB/DIBV5) > HTML Format > Rich Text Format > CF_UNICODETEXT`
      优先级选一种请求。**数据响应不带格式 id**，所以同一时刻只允许一个请求在途（`remoteRequestInFlight_`），
      期间的新列表记为待刷新（`pendingRefresh*`），响应回来再取；分派时严格按请求时的 `kind`，绝不用
      二进制兜底当文本（DIB 头 `28 00 00 00` 按 UTF-16 会变成 `(`）。
    - **本机 html 读取**：不能只看 `getPrimaryHtml()`（只读第一条记录，且富文本往往把 `text/plain` 作主
      MIME、HTML 作附加 Entry）。要遍历记录用 `record.getData('text/html')` 取，再回退文本；不要用
      `getMimeTypes()` 做门控（它可能只列主类型）。
    - **HTML 只加壳、不改内容**：`BuildCfHtml` 若源 html 已自带 `<!--StartFragment-->`/`<!--EndFragment-->`
      （Word/WPS 导出都带）就**复用**，只算字节偏移（CF_HTML 偏移是**字节**，非字符），不注入重复标记、
      不规范化原内容。写本机时若远端只给了片段，用最小 `<html><body>` 包成良构文档；多格式必须落在**同一
      Record 的不同 Entry**（`createData` + `record.addEntry`），不能建两条 Record。
    - **RTF 兜底**：很多 Windows 应用只显式提供 `Rich Text Format` 而无 `HTML Format`（HTML 是 Word 在
      OLE 层按需合成的，rdpclip 用 `EnumClipboardFormats` 枚举**不会触发合成**，客户端只能请求列表内格式），
      故客户端自带 `RtfToHtml`。注意 `\colortbl` 索引约定：第一个 `;` 是索引 0（auto），后面每个 `;` 递增，
      解析时**不要预置空条目**否则 `\cfN` 整体错位、颜色丢失；Word 中文用 `\ab`/`\ai` 表示粗/斜。
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
    指针映射仍以 XComponent 局部坐标为准。左侧为遥测状态区（见第 19 条）：各指标用**固定宽度**
    （`METRIC_WIDTH_*`）避免数字位数变化时重排，间距分「网络↔本机」与其余两档（`METRIC_GAP_*`）。
18. **远端光标同步**（`useRdpCursor`，默认开）：RDP 只传光标**位图**（系统指针仅 `SYSPTR_NULL`/
    `SYSPTR_DEFAULT`），故照搬位图、不做类型映射；HarmonyOS 会把自定义位图缩放到**固定的系统光标大小**，
    所以无需按画面/位图尺寸自行缩放。原生 `HmrdpPostConnect` 用 `graphics_register_pointer` 接管
    `Pointer_Prototype`，`Set` 转 BGRA32 + 去重后经 `kCursorShape` 下发；仅超 256 的位图以**预乘 alpha
    面积平均**缩到 256（热点同比例，源 >1024 丢弃）。UI 用 `Base64Helper` → `createPixelMapSync` →
    `pointer.setCustomCursorSync`，默认/隐藏走 `setPointerStyleSync(DEFAULT)` / `setPointerVisibleSync(false)`
    （模拟器无鼠标，只能真机验证）。关闭开关则不接管、回退默认箭头。
19. **会话状态栏遥测**：原生 `Session::EmitMetrics` 每秒经 `kMetrics` 事件下发
    `rttMs|rxBps|txBps|fps|localUs|responseUs|audioRateHz|audioLossBp`，`SessionPage` 工具栏渲染。
    工具栏在主机名后只显示分辨率（`宽 × 高`），**不再显示解码类型**（H.264 已移除，Progressive
    恒用；`codecMode` 字段与 `OnGfxCodec` 统计已删除）。
    - **网络**：autodetect 的 `NetworkCharacteristicsResult`。FreeRDP **客户端不保存**该值（只有服务端
      注册该回调），故 `Connect` 时给 `context->autodetect` 注册 `HmrdpNetworkCharacteristicsResult` 自行捕获。
    - **本机** = **解码 + 呈现**：gdi 路径下解码链式包裹 `RdpgfxClientContext::SurfaceCommand`、呈现为
      `DrawFrame`；GPU 接管后为引擎 `Compose()` + 上屏（GLES 版 `Renderer::PresentTexture()`；
      Vulkan 版 `GfxVkDesktop::Compose()` + `VkRenderer`，此时 decode 计 0，因为解码已在 GPU）。
      两者按帧平均（`localUs`）。
    - **响应**：RDP 输入与画面是两条**无回显**的流，输入延迟只能推断——仅在**空闲 ≥200ms 后输入、2s 内
      出现首帧**时采样，取近 **5 次均值**；不做该约束会退化成帧节拍。
    - **带宽**：`freerdp_get_stats(context->rdp)` 的收发字节差分（GFX/H.264 下同样有效）。
    - **FPS**：`<系统>/<应用>`，应用=实际呈现数，系统=`display.getDefaultDisplaySync().refreshRate`（VRR 下会变）。
    - **音频**：采样率 + 近期丢帧率；丢帧 = `OnWrite` 欠载补静音 + 环形缓冲溢出的字节，**仅活跃**
      （300ms 内有包）时统计，取近 **5 个窗口**滑动，避免空闲静音误报。
    - **不显示**：服务端处理（协议不回报）、压缩比（仅 GDI 位图路径有意义，GFX/H.264 下不存在）、
      音频丢包（复用在同一传输里，客户端无逐包统计）；音频丢帧率是可感知卡顿的代理。
20. **GPU 桌面引擎 / GFX 接管**（Vulkan：`hmrdp_vk_desktop.{h,cpp}` + `hmrdp_vk_renderer.{h,cpp}`）：
    > ⚠️ **状态（2026-09-14）**：GPU 引擎与上屏**正在整体改为纯 Vulkan**（不留 GLES 残留、保留
    > gdi 回退）；工作包与真机事实见 **`VULKAN-TODO.md`**（唯一交接文档；`PERF-TODO.md` 仅作历史）。
    > 旧的 GLES 实现（`hmrdp_rfx` + `hmrdp_egl` + `hmrdp_renderer`）**已冻结、正确性未成立**，
    > 只作算法/踩坑参考，**随 V7 删除**——不要在 GLES 上做任何新工作。
    >
    > ⚠️ **正确性**：GLES 实现**未达成**"逐像素等于 FreeRDP"——回放「对比」路线实测 `bad≈19/21`
    > （内容级差异），live 开启 GFX 接管后画面异常且会崩溃。因此**不要引用它的任何"验证结论"**；
    > 新后端的目标是 `bad=0`（`VULKAN-TODO.md` §2/§6）。
    **设计口径（Vulkan 版，详见 `VULKAN-TODO.md` §4.2）**：
    - **码流分工**：Progressive / 未压缩 / 表面绘制 → **GPU**（SPIR-V compute + transfer）；
      **ClearCodec 留在 CPU**——复用 FreeRDP 的 `clear_decompress`，通过**共享内存**（持久映射的
      host-visible 表面缓冲）直接读改写表面像素，**不搬 GPU、不做逐区域跨侧往返**。
    - **表面/缓存存储**：持久映射的 host-visible 线性缓冲（`screen` / swapchain 仍是 image），
      于是 CPU 侧访问零成本，不需要 staging / 回读 / 布局状态机。
    - **开关**：全局「硬件解码」（`AppSettings.hardwareDecode`）。关 / 无 Vulkan / 引擎初始化失败 →
      回退 **gdi**。它是**「真机专属功能」**（模拟器不在支持范围，见「AI 助手约定」）。
    - **CPU 写过的表面被 GPU 读取前**必须补 `HOST → TRANSFER` barrier（UMA 不等于免费）。
    - **只做标准能力探测，不做标准 API 的行为自检**（`VULKAN-TODO.md` §3.4）——自检只针对我们自己的
      语义与算法。
    - live 接管由 `hmrdp_session` 的 GPU 分支驱动；`kGpuShadowCompare` 保留**双渲染影子对照**（vs gdi）。
    改动前必读（**与后端无关、必须保留**）：
    - 协议/算法（详见 `VULKAN-TODO.md` §7.1）：**RLGR 必须 64 位读位**；去量化 `shift = quant + progQuant − 1`；
      `RFX_TILE_DIFFERENCE` 用**饱和加法并写回 `current`**；UPGRADE 的 SRL/raw **两个位流同时活跃**；
      每 `(tile,分量)` 的 `current`/`sign`/`bitPos` **跨消息常驻**；compose 必须按桌面尺寸/region clip
      （桌面宽高非 64 倍数，边缘 tile 会按 stride 折回下一行、污染邻接 tile）。
    - **Progressive 必须实现 `update_tiles` 的「同帧图块重复合成」**：FreeRDP 每收到一条 Progressive
      消息，都会用**本消息的 clippingRects** 合成 **`surface->numUpdatedTiles` 里"本帧至今解码过的
      全部 tile"**（`numUpdatedTiles` 只在 frameId 变化时清零 ⇒ 即 RDPGFX StartFrame），而不只是本消息
      的 tile。引擎必须照做（`hmrdp_vk_desktop.cpp` 的 `Surface::frameTiles` + `DecodeProgressive` 的
      reverse job + `rfx_decode.comp` 的 `type==3`），且**必须把 StartFrame 喂给引擎**
      （`GfxMapStartFrame(sink, frameId)`：回放泵 `PmpStartFrame` 与实机 `HmrdpGfxStartFrame` 两条路径
      都要；引擎按 FreeRDP 的规则**只在 frame id 变化时**清列表）。漏掉它 = 一条消息少写一批像素，
      差值会在后续帧累计（`bad` 从 0 涨到 11/16 就是这么来的）。
    - **该重复合成用的 clip 必须取自 REGION 头，且必须是 FreeRDP 的那一份**：FreeRDP 用
      `region16_union_rect()` 把 region rects 合成 `clippingRects`，而它是**带合并**的（同一带内与
      unionRect 相交的项会并成一个**跨越间隙的 bbox 矩形**）—— 所以引擎要**直接调用 FreeRDP 的
      region16 API** 构造同一份集合，不要自己写"原始 rects 并集"。另外**一条 Progressive 消息可以
      "有 REGION、0 个 tile"**（纯重复合成 pass，实测捕获里有 57 字节的这类消息）：此时仍要用该
      region 的 clip 重复合成整帧列表 ⇒ **clip 不能从 tile 推**（`ParseRfxProgressive` 的 `onRegion`
      回调就是为它加的）。
    - ClearCodec **非自包含**（未覆盖像素保留原值）；`SurfaceToCache` 内部会嵌套调用 `EvictCacheEntry`，
      引擎侧要**抑制这次嵌套**；`MapSurfaceToScaledOutput` 本工程不支持（unmap 不合成，与 gdi 现状一致）。
    - `CreateSurface`：宽/高/scanline 按 **16B 对齐**、**0xFF 初始化**、wire `0x20→BGRX32` / `0x21→BGRA32`。
      `SolidFill` 的 alpha 固定 `0xFF`；`SurfaceToSurface` 的 `destPts` 语义按 FreeRDP 实现照搬。
    - compute 派发注意**每轴工作组上限**（`maxComputeWorkGroupCount`，常见 65535）：整屏矩形需要
      10 万+ 工作组，必须用 2D/3D 网格而不是线性下标。
    - **RDPGFX 表面持久**：少实现一条命令，引擎与 gdi 就**永久分叉** ⇒ 捕获回放对比只能在
      Progressive / ClearCodec 补齐后当闸门。
    - 验证回路（跨实现保留）：设置页「抓取 RFX 码流（测试）」→ `hmrdp_gfx.bin`；dev 页「回放测试」
      五条路线（`CPU` / `GLES` / `Vulkan` / `GLES对比` / `Vulkan对比`）；需要**打过补丁并重编的 FreeRDP**
      （`patch-freerdp.ps1` 第 7 步 + `HmrdpSetGfxRawCapture` / `HmrdpGfxReplayNewWithContext`）。
    - **不要每命令排空流水线 / 等待设备**（旧 `DecodeMessage` 末尾 `glFinish` 的教训）：GPU 侧用 barrier，
      只在真正回读 CPU 处同步；**不要立即销毁在飞资源**（fence 延迟回收）。
    - **不要在非目标设备上标定性能参数**：真机口径要压的是**同步点数 / 驱动调用数 / CPU 介入次数**，
      不是模拟器耗时。

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
| `entry/src/main/ets/pages/SessionPage.ets` | XComponent 画面、鼠标/键盘/触屏/触控板输入、浮层、工具栏（左侧状态遥测 + 右侧复制/粘贴、全屏/最小化/断开）、全屏状态跟踪 |
| `entry/src/main/ets/pages/EditConnectionPage.ets` | 新增 / 编辑连接（保存仅写配置，密码连接成功后自动保存；连接按钮下方为可折叠「高级设置」） |
| `entry/src/main/ets/pages/SettingsPage.ets` | 全局设置（工具栏延迟、使用RDP光标、触控板滚动/捏合与触摸模拟、触屏高刷新率、Win 键替代、全局分辨率/缩放、全局连接特性（音频/GFX/H.264/证书）、窗口尺寸与默认最大化、自动隐藏主窗口、配置导入/导出） |
| `entry/src/main/ets/services/ConnectionStore.ets` | 基于 preferences 的连接配置存储（稳定 id + updatedAt，含 `useGlobalDisplay`/`scalePercent`/`useGlobalAdvanced`；`reorder` 持久化拖拽后的 `ids` 顺序） |
| `entry/src/main/ets/services/CredentialStore.ets` | 基于 ASSET 的密码存储（按连接 id，连接成功后写入） |
| `entry/src/main/ets/services/SettingsStore.ets` | 基于 preferences 的设置存储 + 显示解析（`detectedDisplay`/`recommendedScalePercent`/`resolveDisplay`、`SCALE_PRESETS`） |
| `entry/src/main/ets/services/ConfigTransfer.ets` | 配置导入/导出（全局设置 + 全部连接；`DocumentViewPicker` + `fileIo`，密码不导出） |
| `entry/src/main/ets/services/RdpNative.ets` | 每个会话窗口一个实例（独占原生 handle）；按 handle 路由原生事件，`findByKey` 按会话 key 复用实例 |
| `entry/src/main/ets/services/TouchpadWheel.ets` | 触控板 vp 位移 → 高分辨率 RDP 轮转量映射（`TouchpadWheelMapper`，速度倍率、9bit 二补码、0xFF 分片） |
| `entry/src/main/ets/services/DeviceCapabilities.ets` | 运行时设备能力探测（`Capability{supported,reason}`）；音频能力查 `isAudioSupported()`，供设置/编辑页置灰并给出原因。**待补**：模拟器上置灰「硬件解码」与 GPU 回放入口（硬件加速 = 真机专属） |
| `entry/src/main/ets/services/SessionManager.ets` | 主窗口后台连接、每连接状态（转圈/已连接/失败）、错误分类、成功后开窗与断连编排 |
| `entry/src/main/ets/services/WindowController.ets` | 应用窗口默认尺寸、拉起独立会话窗口、会话窗口全屏与系统标题栏/dock 悬停控制、单窗口模式的主窗口隐藏/恢复 |
| `entry/src/main/cpp/hmrdp_napi.cpp` | Node-API 接口 + XComponent surfaceId 绑定 + `isAudioSupported` / `setTouchHighRate` / `setRdpCursor` / `setHardwareDecode` 查询与开关 |
| `entry/src/main/cpp/hmrdp_session.cpp` | FreeRDP 客户端生命周期、输入、事件、光标位图处理、会话遥测（GFX 解码计时 / 带宽采样 / 每秒 `kMetrics`）；GFX 回调喂 GPU 引擎 + 接管/影子对照（`kGpuShadowCompare`） |
| `entry/src/main/cpp/hmrdp_vk_context.{h,cpp}` | Vulkan 上下文：`dlopen("libvulkan.so")` + 标准能力探测 + 进程级 instance/device/queue + 内存类型选择 + 延迟销毁 |
| `entry/src/main/cpp/hmrdp_vk_renderer.{h,cpp}` | Vulkan 上屏：`VK_OHOS_surface` + swapchain + 缩放/letterbox 的 blit |
| `entry/src/main/cpp/hmrdp_vk_desktop.{h,cpp}` | Vulkan 表面引擎：表面注册表 + 命令执行 + 合成 + 屏幕脏区（**V2 起改为 host-visible 缓冲**） |
| `entry/src/main/cpp/cmake/EmbedSpirv.cmake` | GLSL → SPIR-V 的构建期编译/嵌入（用 SDK 自带 `glslang_validator.exe`；shader 列表暂空，V3 起生效） |
| `entry/src/main/cpp/hmrdp_audio.cpp` | `dlopen` OHAudio 的 PCM 播放器（能力探测 + 环形缓冲 + 欠载/溢出丢帧统计 + 中断/错误降级） |
| `entry/src/main/cpp/hmrdp_gfx_capture.{h,cpp}` | 原始通道录制（单文件）：落盘 `hmrdp_gfx.bin`（`u32 长度` + 服务端原始 ZGX 字节）与回放读取（`GfxRawCapture`） |
| `entry/src/main/cpp/hmrdp_gfx_cpu.{h,cpp}` | 离线 FreeRDP CPU（gdi）桌面：复用 FreeRDP 自身解码作为回放的对比路线；`PresentGdiFrame` 为 live gdi 回退与 CPU 回放共用 |
| `entry/src/main/cpp/hmrdp_replay.{h,cpp}` | dev 回放上屏：把 `hmrdp_gfx.bin` 喂给桌面引擎（GLES/Vulkan 由 `GfxReplayRoute` 选择）或离线 gdi 桌面（CPU 路线）并 present 到 XComponent；GPU 路线可叠加 gdi 逐像素对比（固定复现） |
| ⛔ `hmrdp_rfx.{h,cpp}` / `hmrdp_egl.{h,cpp}` / `hmrdp_renderer.{h,cpp}` | **GLES 引擎 / EGL / GLES 渲染器：已冻结，随 V7 删除**，只作算法与踩坑参考 |
