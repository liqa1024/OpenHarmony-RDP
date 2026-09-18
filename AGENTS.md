# AGENTS.md

面向 **鸿蒙 PC（2in1）** 的 RDP 远程桌面客户端（代号 **HmRdp**）。本文件是**入口**：
项目定位、环境、最常用的构建/验证命令，以及**硬规则**。
通用开发知识（协议约束、平台踩坑、验证方法）在 **[`doc_agent/`](doc_agent/README.md)**，按主题分类。

- 应用名：**RDP 远程桌面** · Bundle：`com.lixa.hmrdp` · 目标：HarmonyOS 6.1.0（API 23）
- RDP 引擎：[FreeRDP](https://github.com/FreeRDP/FreeRDP)（源码交叉编译到 `aarch64-linux-ohos` /
  `x86_64-linux-ohos`）；界面 ArkTS/ArkUI，输入经 Node-API 桥接转发；
  **画面由 FreeRDP gdi 出、经呈现器上屏：Vulkan 优先，Vulkan 不能上屏时回落 GLES**
  （`hmrdp_presenter.h`）。「硬件加速」开关**只选上屏后端**（关 = GLES 且不碰 Vulkan，给 Vulkan
  不好用/模拟器兜底），按**呈现能力**置灰（见 [`doc_agent/present-pipeline.md`](doc_agent/present-pipeline.md) §1）。
  自研的 GPU 解码/合成引擎已移除（见 [`doc_agent/gfx-engine.md`](doc_agent/gfx-engine.md) §3）。

## AI 助手约定（硬规则）

- **最终答复/总结一律用中文**（代码、命令、标识符除外）。
- **git 只读**：只允许 `log`/`show`/`diff`/`status`/`blame` 等**查看**操作。**禁止**任何写操作
  （`commit`/`add`/`stash`/`checkout`/`reset`/`restore`/`clean`/`merge`/`rebase`/`branch`/`tag`/`push`/`pull`/`fetch`）；
  要恢复文件就**直接编辑文件内容**，不要用 git 改工作区或历史。
- **实机操作默认禁止，需明确授权，且是最后手段**——唯一例外是 **Vulkan / 硬件加速（上屏）**：
  它们**只在真机上验证**（模拟器的 Vulkan 实现会按标准接口谎报能力，因此**不在支持范围内**），
  这部分由 AI 按 [`doc_agent/gfx-engine.md`](doc_agent/gfx-engine.md) §5/§6 的循环自动执行。
  其余情况下**不要动辄"退回真机"**（上真机麻烦且危险），也**不要把结论甩给真机**。
  签名与安装由用户完成（**仓库不含任何签名材料**），AI 只做无破坏性动作（push 样本 / 启动 / 读日志）。
- **排查顺序**：先怀疑**代码 / 构建 / 数据** → 再怀疑**模拟器自身状态**（跑过重型 GL/GPU 压测后先冷启动）
  → 最后才是真机（且需授权）。
- **改完必须自检**：改 `.ets` 先 `arkts_check`，再 `build_project`；**任务结束前 `build_project` 必须通过**。
- **文档维护**：不要把排查过程、日期、机器/设备型号写进仓库文档。新增结论请写进
  [`doc_agent/`](doc_agent/README.md) 对应的主题文件。
- **`doc_agent/` 是通用知识库，不是记录**（**最容易偏离，写文档前先看这条**）：
  - 写"**结论 + 依据 + 约束**"，一律用**现在时、非人称**陈述，像是本来就该如此——**不要**写
    "这次/本次/我/我们发现/修复前/修复后/已修/待收尾/✅/❌"这类变更与排查语气。
  - **不写机型 / 设备 / 某一份样本 / 某一次运行的数字**（那是本地笔记，放 gitignore 的 `*-TODO*`）。
    量化只给**口径与量级/相对关系**（"整屏帧 ms 级"、"约 1 次/帧"），不给"75→147"这样的流水。
  - 只保留**可复用的判据**：某个量怎么读、多大算正常、改动前必须量什么；**删掉一次性的过程与现场**。
  - 文档改动属**知识订正**：新结论落进对应主题文件（必要时同时修正旧结论），不要把主题文件写成
    按时间累积的日志；条目按主题组织，不按发生顺序组织。

## 文档索引（`doc_agent/`）

| 主题 | 文档 |
|---|---|
| 构建 / 运行 / 真机与模拟器 / uitest 技巧 / 最小验证 | [`build-and-verify.md`](doc_agent/build-and-verify.md) |
| 应用结构（Ability / 页面 / 服务 / 原生模块职责表） | [`architecture.md`](doc_agent/architecture.md) |
| ArkTS / ArkUI 规范与状态坑 | [`arkts-conventions.md`](doc_agent/arkts-conventions.md) |
| 会话窗口、输入映射、工具栏与遥测、剪贴板（手动） | [`session-and-input.md`](doc_agent/session-and-input.md) |
| 设置 / 连接 / 密码（ASSET）/ 导入导出 / 生效时机 | [`settings-and-storage.md`](doc_agent/settings-and-storage.md) |
| 原生库构建与补丁、音频与能力探测 | [`native-libraries.md`](doc_agent/native-libraries.md) |
| GFX 码流框架 / 会话侧接线 / 回放验证回路与**参考画面验收** / **CPU（gdi）链路的成本结构 / 账目口径 / 量测纪律 / 解码侧对拍（§8）**（**改前必读**） | [`gfx-engine.md`](doc_agent/gfx-engine.md) |
| **CPU（gdi）链路的并行与平台适配**（**并行划分原则**、执行器/宽度、流水线分段、相位与内存归属、能量口径、**并行效率量测方法**、已验证与被否证、优化候选） | [`cpu-accel-plan.md`](doc_agent/cpu-accel-plan.md) |
| **（历史，old 弃用）** 旧 CPU 链路文档：只作依据保留，不要从这里接手 | [`cpu-path_old.md`](doc_agent/cpu-path_old.md) |
| 上屏（present）管线：一套实现、Vulkan/GLES 两个后端、零拷贝桌面缓冲与后续工作清单 | [`present-pipeline.md`](doc_agent/present-pipeline.md) |

## 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 在 `<DEVECO_HOME>/sdk/default/openharmony`）。
- 应用 `compatibleSdkVersion` / `targetSdkVersion` 必须与目标设备/模拟器系统版本一致；
  **未经确认不要擅自调整**。
- ABI：`arm64-v8a`（真机，product `default`）、`x86_64`（模拟器，product `emulator`）。
- 宿主为 Windows；OpenSSL 在 WSL 中编译，驱动 Windows 版 OHOS NDK 的 `clang.exe`。

## 构建 / 运行（常用命令）

```bash
devecocli build                                    # product=default → arm64（仅编译）
devecocli build --product emulator                 # 模拟器包（x86_64）
devecocli run --device "<真机序列号>"              # 编译 + 签名 + 安装 + 启动
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"
native/scripts/fetch-sources.ps1                       # 按固定版本拉取三方源码 + 自动打补丁
native/scripts/install-device.ps1 -Device "<序列号>"   # 安装 + 启动（不接触签名材料）
```

- **签名材料不进仓库**：`build-profile.json5` 的 `signingConfigs` 恒为 `[]`，本机签名配置放仓库外
  `.signing/signing-config.json`，由 `hvigorfile.ts` 注入。**切勿**提交 `*.p12`/`*.p7b`/`*.cer`/keystore。
- **三方源码不入库**：`native/third_party/` 由 `fetch-sources.ps1` 按固定版本（tarball + SHA256）拉取。
  改 FreeRDP 一律写成 `patch-steps/` 里的补丁步骤（不要在 `third_party/` 里直接改），
  再 `fetch-sources.ps1 -Force` + 重编 + 把 `.so` 放回 `entry/libs/<abi>/`（该目录**不入库**），
  最后 `build_project`。应用编译用的 FreeRDP/winpr 头文件在 `entry/src/main/cpp/thirdparty/`，
  由 `build-freerdp.ps1` 调用 `sync-freerdp-headers.ps1` 生成（同样**不入库**）。
  详见 [`doc_agent/native-libraries.md`](doc_agent/native-libraries.md)。
- 模拟器优先用于**与硬件加速无关**的功能/逻辑/UI 调试；**Vulkan（硬件加速）只在真机验证**。

## 目录速览

| 路径 | 作用 |
|---|---|
| `entry/src/main/ets/` | ArkUI 界面：`entryability/`、`sessionability/`、`pages/`、`services/`、`model/`、`utils/` |
| `entry/src/main/cpp/` | 原生：FreeRDP 会话、Vulkan/GLES 呈现器、音频、Node-API 桥、dev 回放 |
| `entry/src/main/resources/` | 主题色（`base|dark/element/color.json`）与图标 |
| `native/scripts/` | 原生库源码构建与 FreeRDP 补丁 |
| `entry/libs/<abi>/` | 本地构建的原生库（**gitignore，不入库**） |
| `doc_agent/` | 通用开发知识（见上） |

各模块与页面的**详细职责表**见 [`doc_agent/architecture.md`](doc_agent/architecture.md)。

## 改动前请先读（按主题）

| 要改的东西 | 先读 |
|---|---|
| GFX 码流框架 / 会话侧接线 / 回放与参考画面验收 | [`gfx-engine.md`](doc_agent/gfx-engine.md) |
| CPU（gdi）链路的成本结构 / 账目口径 / 量测纪律 / 对拍 | [`gfx-engine.md`](doc_agent/gfx-engine.md) §8 |
| CPU 解码的并行 / ffrt 执行器与宽度 / 多核与平台适配 | [`cpu-accel-plan.md`](doc_agent/cpu-accel-plan.md) |
| 上屏（present）：Vulkan/GLES 后端、脏区上传、零拷贝桌面缓冲、swapchain 重建 | [`present-pipeline.md`](doc_agent/present-pipeline.md) |
| 输入（鼠标/触屏/触控板/键盘）、会话窗口、工具栏、遥测、剪贴板 | [`session-and-input.md`](doc_agent/session-and-input.md) |
| 设置项、连接存储、密码、导入导出 | [`settings-and-storage.md`](doc_agent/settings-and-storage.md) |
| 页面 / 状态刷新 / 路由 / 主题 | [`arkts-conventions.md`](doc_agent/arkts-conventions.md) |
| FreeRDP 补丁、音频、能力探测、构建选项 | [`native-libraries.md`](doc_agent/native-libraries.md) |
