# 构建 / 签名 / 环境

本文负责构建、签名、安装命令、最小构建门禁和环境。测试与验证流程见 [`testing.md`](testing.md)。

## 1. 产物与 ABI

工程用 **product** 区分两个变体，DevEco Studio 右上角 `Product` 下拉即切换（命令行用 `--product`）：

| product | ABI | 用途 |
|---|---|---|
| `default` | `arm64-v8a` | 真机侧载 |
| `emulator` | `x86_64` | 模拟器调试 |

```bash
devecocli build                                   # = product=default，仅编译
devecocli build --product emulator                # 模拟器包
```

- product → target 的映射在工程级 `build-profile.json5`（`app.products` + `modules[].targets[].applyToProducts`）。
- `devecocli run` 需显式带 target（`--module entry@default` / `entry@emulator`），否则按模块默认 target
  找产物会报 `Build metadata not found`；示例见 §3。
- ABI/库过滤在 `entry/build-profile.json5` 的 `targets[].config.buildOption`：覆盖
  `externalNativeOptions.abiFilters` 与 `nativeLib.filter.excludes`。注意 `abiFilters` **只影响 CMake 编译**
  的 ABI，**不过滤**本地放在 `entry/libs/<abi>/*.so` 的库，所以必须用 `nativeLib.filter.excludes`
  排除另一 ABI 的 `libs/<abi>`。

## 2. 签名（材料不进仓库）

`build-profile.json5` 的 `signingConfigs` **恒为 `[]`**；签名配置放仓库外，由 hvigor 注入：

- `.signing/signing-config.json`（已 gitignore）放本机签名配置（DevEco 自动签名产生的 `type` + `material`，
  口令是 DevEco 的密文，只有 hvigor 能解）；
- 工程级 `hvigorfile.ts` 通过 `config.ohos.overrides.signingConfig` 注入；文件不存在时自动跳过，
  构建仍可用（只出未签名 HAP）。

**DevEco 重新自动签名后**：把它写回的 `material` 覆盖到该文件，并把 `build-profile.json5` 改回 `[]`。
切勿提交 `*.p12` / `*.p7b` / `*.cer` / keystore 或任何签名路径。

## 3. 构建与安装

```powershell
devecocli build                                              # product=default → arm64（hvigor 顺带签名）
devecocli run --module entry@default --device "<序列号>"      # 编译 + 签名 + 安装 + 启动
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"
native/scripts/install-device.ps1 -Device "<序列号>"          # 安装 + 启动（不接触签名材料）
```

- 真机 HAP：`entry/build/default/outputs/default/entry-default-signed.hap`；没有签名配置时是
  `entry-default-unsigned.hap`。用户实机测试直接装这个 HAP，不需要额外步骤。
- `install-device.ps1` 默认 `-Product default`；找不到已签名 HAP 时会提示并只能尝试未签名 HAP。
- 序列号用 `hdc list targets` 查；多设备时 `hdc` 必须带 `-t <serial>`。
- 若 `start_app` / 安装因签名失败：**不要反复重试**，先请用户在 DevEco Studio 里完成签名配置。
- 模拟器占用/关闭、真机例外、日志与 UI 驱动见 [`testing.md`](testing.md)。

## 4. 改动后的最小构建门禁

| 改动 | 必须做 |
|---|---|
| 任何 `.ets` | 先 `arkts_check <文件>`（快），再 `build_project` |
| 任何 `.cpp/.h/.vert/.frag` | `build_project`（任务结束前必须通过） |
| 改 FreeRDP 源码/补丁 | 见 [`native-build.md`](native-build.md)（构建 / 补丁体系）与 [`rdp-native.md`](rdp-native.md) §3（补丁内容）：重打补丁 + 重编 + 把 `.so` 放回 `entry/libs/<abi>/`，再 `build_project` |
| 改 GFX / 解码 / 回放 | 见 [`rdp-gfx.md`](rdp-gfx.md) §6 的**回放与参考画面**（仅 RDP 解码/合成；不覆盖呈现器/超分；Vulkan 上屏只在真机）；测试分层见 [`testing.md`](testing.md) |
| 改输入 / 会话窗口 | 测试与验证见 [`testing.md`](testing.md)（模拟器/真机分层、UI 判断归属） |

- **这里的门禁只管“能不能交付构建”，不等于 UI/功能验收**；验证分层、真机/模拟器流程、日志与交接见
  [`testing.md`](testing.md)。
- 改 `.ets` 后**不要**只看编译通过：ArkUI 的坑多数是**运行时**的（见 [`arkts-conventions.md`](arkts-conventions.md)）。

## 5. 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 在 `<DEVECO_HOME>/sdk/default/openharmony`）。
  缺失时构建/静态检查工具不可用。
- **SDK 版本口径**：`targetSdkVersion` 决定"能编译哪些 API"，`compatibleSdkVersion` 决定"能装到多老的系统"。
  工程取 `targetSdkVersion` = `26.0.0`、`compatibleSdkVersion` = `6.1.0(23)`：新版 ArkUI 能力（如沉浸光感）
  可以编译，同时保留 API 23 设备/模拟器的安装与运行。**任何一方的数值未经确认不要擅自调整**。
- **版本号写法在 API 26 处换过一次**：API 26 之前写 `M.S.F(API)`（如 `6.1.0(23)`），API 26 起写纯 `M.S.F`
  （如 `26.0.0`）。写成 `26.0.0(26)` 会被 hvigor 拒绝（`api version parameter is illegal`）。
- **`compatibleSdkVersion` 低于所用 API 的 `since` 时**：编译器对每次调用报
  "It is recommended to use apiAvailable to safeguard API compatibility" 告警。
  这类调用必须做运行时版本保护：先按底座版本号兜住，再用 `if (deviceInfo.apiAvailable('26.0.0'))`
  （版本号必须是**字面量、直接写在 if 里**）包住新 API；否则在老系统上是**直接崩溃**而不是降级。
- **ArkTS 编译告警按成因处理**（`build_project` 输出，WARN）：API 声明了 `@throws` 的调用若未用
  `try/catch`（或 Promise 的 `.catch`）包住，会报 `Function may throw exceptions. Special handling is
  required.`；多设备（syscap）/ 权限告警用
  `@SuppressWarnings({rules: [SuppressWarningsType.SYSCAP | PERMISSION]})`（`@ohos.annotation`，API 23+；
  装饰器对类 / 方法生效，**不能**标自由函数）标注承载调用的类或方法消除。两类告警的具体口径见
  [`audio.md`](audio.md) §7、[`clipboard.md`](clipboard.md) §2。
- **`Currently module for 'libhmrdp.so' is not verified` 属误报**：本机 Node-API 库的类型验证告警
  （类型已由 `entry/src/main/cpp/types/libhmrdp/index.d.ts` 提供，引用处类型正常解析），源码侧无法消除，
  不必处理。
- 宿主为 Windows；原生依赖全部用 CMake 驱动 Windows 版 OHOS NDK 的 `clang.exe` 构建，
  **日常构建不涉及 WSL/Perl**（WSL 只在重出 OpenSSL 的 `perl Configure` 快照时用到）。

## 6. 相关文档

- 测试与验证（AI 自测、默认流程、用户 UI 验收、AI 实机例外、用户操作 + hilog、日志、UI 驱动、交接）：
  [`testing.md`](testing.md)
- 原生构建与补丁：[`native-build.md`](native-build.md)
- 回放与参考画面：[`rdp-gfx.md`](rdp-gfx.md) §6/§7
- 文档归属与阅读顺序：[`README.md`](README.md)
