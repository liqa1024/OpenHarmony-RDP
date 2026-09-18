# 构建 / 运行 / 验证

## 1. 产物与 ABI

工程用 **product** 区分两个变体，DevEco Studio 右上角 `Product` 下拉即切换（命令行用 `--product`）：

| product | ABI | 用途 |
|---|---|---|
| `default` | `arm64-v8a` | 真机侧载 |
| `emulator` | `x86_64` | 模拟器调试 |

```bash
devecocli build                                   # = product=default，仅编译
devecocli build --product emulator                # 模拟器包
devecocli run --device "<真机序列号>"             # 编译 + 签名 + 安装 + 启动
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"
```

- product → target 的映射在工程级 `build-profile.json5`（`app.products` + `modules[].targets[].applyToProducts`）。
- `devecocli run` 需显式带 target（否则按模块默认 target 找产物会报 `Build metadata not found`）。
- ABI/库过滤在 `entry/build-profile.json5` 的 `targets[].config.buildOption`：覆盖
  `externalNativeOptions.abiFilters` 与 `nativeLib.filter.excludes`。注意 `abiFilters` **只影响 CMake
  编译**的 ABI，**不过滤**本地放在 `entry/libs/<abi>/*.so` 的库，所以必须用
  `nativeLib.filter.excludes` 排除另一 ABI 的 `libs/<abi>`。

## 2. 签名（材料不进仓库）

`build-profile.json5` 的 `signingConfigs` **恒为 `[]`**；签名配置放仓库外，由 hvigor 注入：

- `.signing/signing-config.json`（已 gitignore）放本机签名配置（DevEco 自动签名产生的 `type` +
  `material`，口令是 DevEco 的密文，只有 hvigor 能解）；
- 工程级 `hvigorfile.ts` 通过 `config.ohos.overrides.signingConfig` 注入；文件不存在时自动跳过，
  构建仍可用（只出未签名 HAP）。

**DevEco 重新自动签名后**：把它写回的 `material` 覆盖到该文件，并把 `build-profile.json5` 改回 `[]`。
切勿提交 `*.p12` / `*.p7b` / `*.cer` / keystore 或任何签名路径。

## 3. 改动后的最小验证（改动类型 → 该跑什么）

| 改动 | 必须做 |
|---|---|
| 任何 `.ets` | 先 `arkts_check <文件>`（快），再 `build_project` |
| 任何 `.cpp/.h/.vert/.frag` | `build_project`（任务结束前必须通过） |
| 改 FreeRDP 源码/补丁 | 见 [`native-libraries.md`](native-libraries.md)：重打补丁 + 重编 + 把 `.so` 放回 `entry/libs/<abi>/`，再 `build_project` |
| 改 GFX / 解码 / 回放 | 见 [`gfx-engine.md`](gfx-engine.md) §6 的**回放验证回路**（回放 + `bad=0`；Vulkan 上屏只在真机） |
| 改输入 / 会话窗口 | 模拟器（2in1）上手工走一遍；光标同步、真机 GPU 相关例外见对应文档 |

- **验证纪律**：先怀疑**代码 / 构建 / 数据** → 再怀疑**模拟器自身状态**（跑过重型 GL/GPU 压测后先冷启动）
  → 最后才是真机（且需授权）。
- 改 `.ets` 后**不要**只看编译通过：ArkUI 的坑多数是**运行时**的（见 [`arkts-conventions.md`](arkts-conventions.md)）。

## 4. 真机流程

```powershell
devecocli build                                       # product=default → arm64（hvigor 顺带签名）
native/scripts/install-device.ps1 -Device "<序列号>"   # 安装 + 启动（不接触签名材料）
hdc -t <序列号> shell "hilog -x -D 0xD001"            # 读本应用的原生日志
```

- 序列号用 `hdc list targets` 查；多设备时 `hdc` 必须带 `-t <serial>`。
- 若 `start_app` 因签名/未配置签名失败：**不要反复重试**，先请用户在 DevEco Studio 里完成签名配置。
- 应用日志：原生用 `hmrdp_log.h` 的 `HMRDP_LOGI/W/E`（domain `0xD001`，tag `HmRdpNative`）。

## 5. 模拟器（功能测试）

- 首选 **2in1 模拟器**（与目标系统同版本），用于**与硬件加速无关**的调试。
- 启动：`devecocli emulator start "<模拟器名>"`；同一时刻只有一个实例占用 5555。
- **Vulkan / 硬件加速（上屏）是"真机专属功能"，模拟器不在支持范围内**（模拟器的 Vulkan 实现会按
  标准接口谎报能力），所以不要在模拟器上开硬件加速，也不要用模拟器结论约束真机行为。

### 5.1 在设备上驱动 UI（可靠用法）

- `uitest uiInput click <x> <y>` 注入的是**触摸**事件，**不会**触发 `onMouse`；鼠标请用 `uinput -M ...`，
  但它的 `-m` 是**相对/累加**移动、指针常不可见，精确定位不可靠。
- 精确坐标：`uitest dumpLayout -p /data/local/tmp/layout.json` + `hdc file recv`，按控件 `bounds` 算中心点。
- ⚠ **别用盲点坐标**：会话窗口是**可缩放的**（安装后首次进入通常不是全屏），按全屏推算的坐标会落到
  桌面或别的窗口上——**症状是"点了没反应"，甚至把应用窗口关掉/切走**，看起来像"应用崩了"。
  控件定位请在**每次切页面后重新 dump**，比目测截图可靠。
- 截图：`hdc shell snapshot_display -f /data/local/tmp/x.jpeg` + `hdc file recv`。
- 读日志前先 `hilog -r` 清缓冲。**周期性统计行会很快冲掉缓冲区**，关键结论行若只在早期打印，
  应尽早抓取或让代码在收尾时重打一次。
- **新增日志要自己拼成字符串**：hilog 对没有 `%{public}` 标记的转换说明符一律输出 `<private>`
  （`%d`/`%u`/`%s` 全都中招），随手加的 `HMRDP_LOGW("... %d", v)` 打出来是一行 `<private>`。
  要么每个参数都写 `%{public}`，要么先 `snprintf` 成一行再用 `%{public}s` 打。

### 5.2 标准多轮回放（dev 页）

dev 页的 stats 文本**第一行**是状态与轮次号：

```
state=running|finished|aborted  run=<n>  route=…  mode=…  frames=…  fps=…
```

多轮测量一律用 `native/scripts/replay-rounds.ps1`，不要手动点：

```powershell
native/scripts/replay-rounds.ps1 -Device "<序列号>" -Capture .cache/hmrdp_gfx_video.bin `
    -RefTag fefd78fd -Rounds "参考:关","参考:对比" -Out .cache/rounds.txt
```

- 脚本等**自己这一轮**的 `run` 跑到 `finished` 再动下一步；**一轮没跑完绝不点下一次**——模式按钮
  （参考/节拍/路线/线程）内部是 `stop + start`，中途点击 = 掐断，而掐断的轮次**不是测量**
  （`state=aborted` 会被报出来，且不会写参考）。
- 别按"看起来没在跑"判断：两轮的 stats 文本长得一样，按内容猜会把数据记到上一轮头上（`run=` 为此存在）。
- 脚本用坐标点击 ⇒ 窗口必须在最前（脚本先 `aa start`）；点击没生效会直接报
  `did not start a new run`，而不是干等到超时。
- 参考的本地副本在 `.cache/hmrdp_ref_<captureTag>.{hash,bmp}`（不入库）；**解码侧改动后要重录**，
  判据见 [`gfx-engine.md`](gfx-engine.md) §8.1/§8.4。

## 6. 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 在 `<DEVECO_HOME>/sdk/default/openharmony`）。
  缺失时构建/静态检查工具不可用。
- 应用 `compatibleSdkVersion` / `targetSdkVersion` 必须与目标设备/模拟器的系统版本一致；
  **未经确认不要擅自调整**。
- 宿主为 Windows；OpenSSL 在 WSL 内编译，驱动 Windows 版 OHOS NDK 的 `clang.exe`。
