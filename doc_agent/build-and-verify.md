# 构建 / 运行 / 验证

## 1. 产物与 ABI

工程用 **product** 区分两个变体，DevEco Studio 右上角 `Product` 下拉即切换（命令行用 `--product`）：

| product | target | ABI | 用途 |
|---|---|---|---|
| `default` | `entry@default` | `arm64-v8a` | 真机侧载 |
| `emulator` | `entry@emulator` | `x86_64` | 模拟器调试 |

```bash
devecocli build                                   # = product=default，仅编译
devecocli build --product emulator                # 模拟器包
devecocli run --device "<真机序列号>"             # 编译 + 签名 + 安装 + 启动
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"
```

- product → target 的映射在工程级 `build-profile.json5`（`app.products` + `modules[].targets[].applyToProducts`）。
- `devecocli run` 需显式带 target（否则按模块默认 target 找产物会报 `Build metadata not found`）。
- 两个 target 的 ABI/库过滤在 `entry/build-profile.json5` 的 `targets[].config.buildOption`：覆盖
  `externalNativeOptions.abiFilters` 与 `nativeLib.filter.excludes`（互斥排除另一 ABI 的 `libs/<abi>`）。
  注意 `abiFilters` **只影响 CMake 编译**的 ABI，**不过滤**本地放在 `entry/libs/<abi>/*.so` 的库，
  所以必须用 `nativeLib.filter.excludes` 排除。

## 2. 签名（材料不进仓库）

`build-profile.json5` 的 `signingConfigs` **恒为 `[]`**；签名配置放仓库外，由 hvigor 注入：

- `.signing/signing-config.json`（已 gitignore）放本机签名配置（DevEco 自动签名产生的 `type` + `material`，
  口令是 DevEco 的密文，只有 hvigor 能解）；
- 工程级 `hvigorfile.ts` 通过 `config.ohos.overrides.signingConfig` 注入（官方《动态修改编译配置》方式二）；
  文件不存在时自动跳过，构建仍可用（只出未签名 HAP）。

**DevEco 重新自动签名后**：把它写回的 `material` 覆盖到该文件，并把 `build-profile.json5` 改回 `[]`。
切勿提交 `*.p12` / `*.p7b` / `*.cer` / keystore 或任何签名路径。

## 3. 改动后的最小验证（改动类型 → 该跑什么）

| 改动 | 必须做 |
|---|---|
| 任何 `.ets` | 先 `arkts_check <文件>`（快），再 `build_project` |
| 任何 `.cpp/.h/.comp` | `build_project`（任务结束前必须通过） |
| 改 FreeRDP 源码/补丁 | 见 [`native-libraries.md`](native-libraries.md)：重打补丁 + 重编 + 把 `.so` 放回 `entry/libs/<abi>/`，再 `build_project` |
| 改 GFX / Progressive / GPU 引擎 | 见 [`gfx-engine.md`](gfx-engine.md) 的**回放验证回路**（真机回放 + `bad=0`） |
| 改输入 / 会话窗口 | 模拟器（2in1）上手工走一遍；光标同步、触控板、真机 GPU 相关例外见对应文档 |

- **验证纪律**：先怀疑**代码 / 构建 / 数据**，再怀疑**模拟器自身状态**（跑过重型 GL/GPU 压测后先冷启动），
  最后才是真机。
- 改 `.ets` 后**不要**只看编译通过：ArkUI 的坑多数是**运行时**的（见 [`arkts-conventions.md`](arkts-conventions.md)）。

## 4. 真机流程

```powershell
devecocli build                                   # product=default → arm64（hvigor 顺带签名）
native/scripts/install-device.ps1 -Device "<序列号>"   # 安装 + 启动（不接触签名材料）
hdc -t <序列号> shell "hilog -x -D 0xD001"        # 读本应用的原生日志（domain 0xD001）
```

- 序列号用 `hdc list targets` 查；多设备时 `hdc` 必须带 `-t <serial>`。
- 若 `start_app` 在真机上因签名/未配置签名失败：**不要反复重试**，先请用户在 DevEco Studio 里完成签名配置。
- 应用日志：原生用 `hmrdp_log.h` 的 `HMRDP_LOGI/W/E`（domain `0xD001`，tag `HmRdpNative`）。

## 5. 模拟器（功能测试）

- 首选 **2in1 模拟器**（与目标系统同版本），用于**与硬件加速无关**的调试。
- 启动：`devecocli emulator start "<模拟器名>"`（名字取自本机已安装的模拟器列表）；同一时刻只有一个实例占用 5555。
- **Vulkan / 硬件加速 / GPU 回放是"真机专属功能"，模拟器不在支持范围内**（模拟器的 Vulkan 实现会按标准接口
  谎报能力），所以**不要**在模拟器上开硬件加速或跑 GPU 回放，也不要用模拟器结论约束真机行为。

### 5.1 在设备上驱动 UI（可靠用法）

- `uitest uiInput click <x> <y>` 注入的是**触摸**事件，**不会**触发 `onMouse`；鼠标请用 `uinput -M ...`，
  但它的 `-m` 是**相对/累加**移动、指针常不可见，精确定位不可靠。
- 精确坐标：`uitest dumpLayout -p /data/local/tmp/layout.json` + `hdc file recv`，按控件 `bounds` 算中心点。
  比目测截图可靠得多。
- 截图：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg` + `hdc file recv`。
- 读日志前先 `hilog -r` 清缓冲；`hilog -x -D 0xD001` 只取本应用的原生 domain。
  **周期性统计行会很快冲掉缓冲区**，关键结论行若只在早期打印，应尽早抓取或让代码在收尾时重打一次。

## 6. 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 在 `<DEVECO_HOME>/sdk/default/openharmony`）。
  缺失时构建/静态检查工具不可用。
- 应用 `compatibleSdkVersion` / `targetSdkVersion` 必须与目标设备/模拟器的系统版本一致；
  **未经确认不要擅自调整**。
- 宿主为 Windows；OpenSSL 在 WSL 内编译，驱动 Windows 版 OHOS NDK 的 `clang.exe`。
