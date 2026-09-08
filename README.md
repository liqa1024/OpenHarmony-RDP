# RDP 远程桌面

面向 **鸿蒙 PC（2in1）** 的远程桌面（RDP）客户端，基于 **FreeRDP 3.10.3** 从源码交叉编译，
使用 **ArkUI + Node-API + EGL/OpenGL ES** 实现原生渲染与输入转发。

## 特性

- **RDP 协议栈**：FreeRDP 3.10.3（从源码交叉编译），支持 NLA/CredSSP、TLS
- **图形管道**：RDPGFX（RemoteFX / 渐进式），EGL/GLES 纹理上传 + GPU 等比缩放
- **输入**：鼠标（移动/左中右键/滚轮）、键盘（扫描码 + Unicode）、触屏（RDPEI 原生触屏转发）
- **触控板**：双指滚动映射为滚轮（含横向）、双指捏合映射为 Ctrl+滚轮 缩放，步长/灵敏度可配置
- **音频**：rdpsnd（OpenSLES 后端）
- **剪贴板**：cliprdr 通道已接入
- **凭证安全存储**：密码经 HarmonyOS Asset Store Kit 密文存储（不落明文、不进日志），
  且**仅在连接验证成功后**才写入；连接配置与密码分离存储
- **连接管理**：书签式列表、右键菜单（连接/编辑/删除）、一键直连；连接配置随时可存，
  无密码也能保存；每个连接可选「高级设置」（独立分辨率/缩放、剪贴板、音频、GFX、H.264 等）
- **显示自适应**：默认自动取当前显示器分辨率并推荐 Windows 缩放档位
  （100/125/150/175/200/225）；可在全局设置中改为手动分辨率/缩放，也可在单个连接中覆盖
- **深浅色主题**：跟随系统
- **会话工具栏**：鼠标停留屏幕顶部自动滑出，提供「全屏/退出全屏」「最小化」「断开」；
  窗口模式下默认隐藏（可在设置中开启），全屏模式下始终显示
- **多窗口会话**：连接后在独立的 `SessionAbility` 主窗口中打开远程桌面；最大化即沉浸式全屏
  （隐藏系统标题栏 / dock），可最小化、退出全屏并自由拖动缩放
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
│  WindowController（窗口尺寸 / 拉起 / 全屏控制）              │
├─────────────────────────────────────────────────────────────┤
│  Node-API Bridge  (entry/src/main/cpp/hmrdp_napi.cpp)       │
├─────────────────────────────────────────────────────────────┤
│  Session wrapper  (hmrdp_session.cpp)                       │
│  EGL/GLES renderer(hmrdp_renderer.cpp)                      │
├─────────────────────────────────────────────────────────────┤
│  FreeRDP 3.10.3   libfreerdp3 / libwinpr3 / libfreerdp-client3 │
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
    services/                 ConnectionStore · CredentialStore · SettingsStore · RdpNative · WindowController
  src/main/cpp/               NAPI 桥接 + FreeRDP 封装 + EGL 渲染器
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

### 从源码重建原生库（可选）

`entry/libs/` 已包含构建好的库；如需自行编译：

```powershell
# 1. 下载 FreeRDP 3.10.3 到 native/third_party/FreeRDP 并打补丁
pwsh native/scripts/patch-freerdp.ps1

# 2. 编译 OpenSSL（在 WSL 中，用现有 Windows OHOS NDK 作为交叉编译器）
wsl -d Ubuntu -e bash -lc 'bash native/scripts/build-openssl-wsl.sh \
  <openssl-src> <build-dir> <install-dir> arm64-v8a'

# 3. 编译 FreeRDP
pwsh native/scripts/build-freerdp.ps1 -Arch arm64-v8a
```

依赖：OpenSSL 3.0.15、zlib 1.3.1（均从源码编译，静态链接进 FreeRDP）。

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

本项目采用 [MIT 许可证](LICENSE)。
