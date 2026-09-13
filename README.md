# RDP 远程桌面

面向 **鸿蒙 PC（2in1）** 的远程桌面（RDP）客户端，基于 **FreeRDP 3.10.3** 从源码交叉编译，
使用 **ArkUI + Node-API + EGL/OpenGL ES** 实现原生渲染与输入转发。

## 特性

- **RDP 协议栈**：FreeRDP 3.10.3（从源码交叉编译），支持 NLA/CredSSP、TLS
- **图形管道（GPU 接管）**：RDPGFX（RemoteFX / 渐进式）。默认由 **GPU 桌面引擎**接管——CPU 只做 ZGFX +
  命令解析，渐进 / 未压缩解码、多表面合成与上屏都在 **GLES 3.1 compute** 上完成，引擎屏幕以**共享 EGL
  纹理**直连上屏（无 CPU 读回 / 上传）；ClearCodec 复用 FreeRDP 解码器做 CPU 钩子。设置页「硬件解码」
  关闭、或设备无 GLES 3.1 compute / 引擎初始化失败时，自动回退 FreeRDP gdi（CPU 软解）
- **输入**：鼠标（移动/左中右键/滚轮）、键盘（扫描码 + Unicode）、触屏（RDPEI 原生触屏转发，含接触
  压力；可选「高刷新率」解除 FreeRDP 的 50Hz 帧合并）
- **光标同步**：远端光标形状（文本、手型、窗口边缘缩放等）映射为鸿蒙系统光标，大光标自动缩放到 256；
  全局设置「使用RDP光标」可关闭（默认开，重开会话生效）
- **触控板**：双指滚动映射为高分辨率滚轮（含横向，速度可调）；双指捏合默认映射为 Ctrl+滚轮 缩放，
  可开启「使用触摸模拟触控板捏合」改为在鼠标位置合成原生双指触摸（角度、初始距离可调，缩放 1:1）
- **音频**：rdpsnd 通道由 FreeRDP 解码，`libhmrdp` 用**原生 OHAudio** 播放；设备无音频能力时
  自动关闭，并在设置中置灰说明原因
- **剪贴板（手动同步）**：cliprdr 通道已接入。同步**全程由工具栏「复制/粘贴」手动触发**，
  刻意不做自动同步——既能免申请剪贴板读取权限（粘贴用系统 `PasteButton` 临时授权），
  也让大数据量传输可控、两个方向逻辑一致。支持 **文本 / 富文本（HTML）/ 图片**双向；
  远端只提供 RTF（Word 之类不显式给 HTML）时自动转成 HTML，尽量保留颜色、粗斜体、下划线、
  删除线等格式；文件传输预留。每次操作按实际内容类型给出提示（文本/富文本/图片）
- **凭证安全存储**：密码经 HarmonyOS Asset Store Kit 密文存储（不落明文、不进日志），
  且**仅在连接验证成功后**才写入；连接配置与密码分离存储
- **连接管理**：书签式列表、右键菜单（连接/编辑/删除）、一键直连；连接配置随时可存，
  无密码也能保存；每个连接可选「高级设置」（独立分辨率/缩放、剪贴板、音频、证书校验等）
- **配置备份**：可将全局设置与全部连接导出为 JSON 文件，或从文件导入覆盖；
  密码保存在系统安全存储中，导出时跳过并提示，导入后需重新输入
- **显示自适应**：默认自动取当前显示器分辨率并推荐 Windows 缩放档位
  （100/125/150/175/200/225）；可在全局设置中改为手动分辨率/缩放，也可在单个连接中覆盖
- **深浅色主题**：跟随系统
- **会话工具栏**：窗口模式下始终显示；全屏模式下鼠标停留屏幕顶部自动滑出。右侧按钮依次为
  「复制/粘贴」「全屏/退出全屏」「最小化」「断开」。剪贴板语义：「复制」＝远端→本机、
  「粘贴」＝本机→远端，完成后按内容类型提示（文本 / 富文本 / 图片）
- **会话状态栏（实时遥测）**：工具栏左侧显示「网络（RTT）/ 本机（解码+呈现）/ 响应（输入→画面）/
  FPS（系统刷新率/应用刷新率）/ 上下行带宽」，音频开启时另显示「采样率 + 近期丢帧率」。网络与本机
  按阈值着色（网络宽松、本机严格），响应取近期均值但不定色；指标每秒刷新一次
- **多窗口会话**：连接后在独立的 `SessionAbility` 主窗口中打开远程桌面；最大化即沉浸式全屏
  （隐藏系统标题栏 / dock），可最小化、退出全屏并自由拖动缩放
- **自动隐藏主窗口（单窗口模式）**：全局设置可开启（默认关）；连接后关闭主窗口、桌面只保留
  会话窗口，关闭会话后主窗口自动恢复。该模式按**单会话**设计，不支持多会话并存
- **窗口尺寸可配置**：主窗口 / 会话窗口的默认尺寸按屏幕分辨率比例推导（45% / 67%），
  可在设置中开关并调整；会话窗口还可设为默认最大化（全屏）

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  ArkTS / ArkUI  (entry/src/main/ets)                        │
│  Index · SessionPage · EditConnectionPage · SettingsPage    │
│  EntryAbility · SessionAbility（独立会话窗口）              │
│  ConnectionStore(preferences) · CredentialStore(ASSET)      │
│  SettingsStore（设置 + 分辨率/缩放解析）                     │
│  ConfigTransfer（配置导入/导出，密码不导出）                 │
│  WindowController（窗口尺寸 / 拉起 / 全屏控制）              │
├─────────────────────────────────────────────────────────────┤
│  Node-API Bridge  (entry/src/main/cpp/hmrdp_napi.cpp)       │
├─────────────────────────────────────────────────────────────┤
│  Session wrapper  (hmrdp_session.cpp)                       │
│  GPU desktop engine  (hmrdp_rfx_gpu / hmrdp_gfx_desktop)    │
│  EGL/GLES renderer + EGL share  (hmrdp_renderer / egl)      │
│  OHAudio output   (hmrdp_audio.cpp, dlopen libohaudio)      │
├─────────────────────────────────────────────────────────────┤
│  FreeRDP 3.10.3   libfreerdp3 / winpr3 / client3            │
│  (entry/libs/<abi>, OpenSSL + zlib 静态链接)                  │
└─────────────────────────────────────────────────────────────┘
```

## 目录结构

```
AppScope/                     应用级配置与图标
entry/
  libs/<abi>/                 FreeRDP 预编译动态库（arm64-v8a / x86_64）
  src/main/ets/               ArkTS 界面与业务
    entryability/             EntryAbility（主窗口）
    sessionability/           SessionAbility（独立会话窗口）
    pages/                    Index · SessionPage · EditConnectionPage · SettingsPage
    services/                 ConnectionStore · CredentialStore · SettingsStore · ConfigTransfer · RdpNative · TouchpadWheel · SessionManager · WindowController · DeviceCapabilities
  src/main/cpp/               NAPI 桥接 + FreeRDP 封装 + GPU 桌面引擎 + EGL 渲染器 + OHAudio 播放
  src/main/cpp/thirdparty/    FreeRDP 头文件
  src/main/resources/         资源（含 dark 深色变体）
native/
  scripts/                    原生库从源码重建脚本
  third_party/                下载的源码与构建产物（gitignore）
```

## 构建

### 前置条件

- DevEco Studio（含 HarmonyOS SDK / NDK）
- `DEVECO_HOME` 指向 DevEco Studio 安装目录
- 目标设备/模拟器：HarmonyOS 6.1.0（API 23）

### 构建应用

用 DevEco Studio 打开工程直接运行，或使用命令行：

```bash
devecocli build            # 编译
devecocli run --device <serial>   # 编译 + 安装 + 启动
```

> 仓库不含签名材料。首次构建请在 DevEco Studio 中配置自动签名，或运行
> `devecocli signature generate` 生成本地调试签名。
>
> 若实机安装报 `PathExistsException`（路径含 `app_icon.png`）：这是 DevEco 安装器的图标缓存
> 问题，不影响运行。完全退出 DevEco、删除 `%LOCALAPPDATA%\Temp\hap_installer` 后重启再装即可。

### 从源码重建原生库（可选）

`entry/libs/` 已包含构建好的库；如需自行编译：

```powershell
# 1. 下载 FreeRDP 3.10.3 到 native/third_party/FreeRDP 并打补丁
pwsh native/scripts/patch-freerdp.ps1

# 2. 编译 zlib（静态库，被 FreeRDP 静态链入）
pwsh native/scripts/build-zlib.ps1 -Arch arm64-v8a

# 3. 编译 OpenSSL（在 WSL 中，用现有 Windows OHOS NDK 作为交叉编译器）
wsl -d Ubuntu -e bash -lc 'bash native/scripts/build-openssl-wsl.sh \
  <openssl-src> <build-dir> <install-dir> arm64-v8a'

# 4. 编译 FreeRDP
pwsh native/scripts/build-freerdp.ps1 -Arch arm64-v8a

# 5. 把产物放到 entry/libs/<abi>/（该目录已 gitignore）
copy native/install/arm64-v8a/freerdp/lib/*.so entry/libs/arm64-v8a/
```

依赖：OpenSSL 3.0.15、zlib 1.3.1（均从源码编译，静态链接进 FreeRDP）。

> 换 ABI 只需把 `arm64-v8a` 换成 `x86_64`（模拟器）并把产物放到 `entry/libs/x86_64/`；
> 三个脚本都接受 `-Arch`/架构参数，装到 `native/install/<abi>/`。
> 脚本按顺序有依赖：zlib / OpenSSL 必须先于 FreeRDP。

## 命名

| 项 | 值 |
|---|---|
| 应用名称 | RDP 远程桌面 |
| Bundle 名称 | `com.lixa.hmrdp` |
| 兼容版本 | HarmonyOS 6.1.0（API 23） |
| 支持 ABI | arm64-v8a（真机）、x86_64（模拟器） |

## 第三方

| 组件 | 版本 | 许可证 |
|---|---|---|
| [FreeRDP](https://www.freerdp.com/) | 3.10.3 | Apache-2.0 |
| [OpenSSL](https://www.openssl.org/) | 3.0.15 | Apache-2.0 |
| [zlib](https://zlib.net/) | 1.3.1 | zlib |

## 许可证

Copyright (C) 2026 Qing'an Li

本项目采用 [GNU 通用公共许可证第 3 版或更新版本](LICENSE)（`GPL-3.0-or-later`）。

本程序是自由软件：你可以依据自由软件基金会发布的 GNU 通用公共许可证条款重新发布和/或修改它，
无论是该许可证的第 3 版，还是（你选择的）任何更新的版本。

本程序的发布是希望其有用，但不提供任何担保，甚至没有对适销性或特定用途适用性的默示担保；
详见 GNU 通用公共许可证。你应已随本程序收到一份 GNU 通用公共许可证副本，
若没有请参见 <https://www.gnu.org/licenses/>。

第三方组件（FreeRDP、OpenSSL、zlib）仍遵循各自的原始许可证，不受本项目授权变更影响。
