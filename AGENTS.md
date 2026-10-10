# AGENTS.md

面向 **鸿蒙 PC（2in1）** 的 RDP 远程桌面 / 云串流客户端（代号 **HmRdp**，Bundle `com.lixa.hmrdp`）。
本文件是**入口**：项目定位、硬规则、环境与常用命令，以及「改什么读什么」的文档地图。
**通用开发知识在 [`doc_agent/`](doc_agent/README.md)**（按主题分类，含归属表与写作约定）。
面向使用者的功能、架构图、许可见根 [`README.md`](README.md)。

- 两条协议并列：**RDP**（FreeRDP 源码交叉编译）与 **Moonlight / Sunshine**（`moonlight-common-c`）；
  尽量共用连接列表、会话外壳与输入分流。
- 界面 ArkTS/ArkUI，原生经 Node-API 桥接。RDP 画面由 FreeRDP **gdi** 出、经呈现器上屏
  （**Vulkan 优先，GLES 兜底**）；Moonlight 默认解码**直出 surface**，仅超分链路复用 RDP 的 Vulkan 呈现器。

## 硬规则

- **最终答复/总结一律用中文**（代码、命令、标识符除外）。
- **git 只读**：只允许 `log`/`show`/`diff`/`status`/`blame` 等**查看**操作。**禁止**任何写操作
  （`commit`/`add`/`stash`/`checkout`/`reset`/`restore`/`clean`/`merge`/`rebase`/`branch`/`tag`/`push`/`pull`/`fetch`）；
  要恢复文件就**直接编辑文件内容**。
- **实机操作默认禁止，需明确授权，且是最后手段**——唯一例外是 **Vulkan / 硬件加速（上屏）**：
  它们**只在真机上验证**（模拟器的 Vulkan 会按标准接口谎报能力，不在支持范围内），由 AI 按
  [`doc_agent/rdp-gfx.md`](doc_agent/rdp-gfx.md) §6 与 [`doc_agent/rdp-present.md`](doc_agent/rdp-present.md) §8 自动执行。
  其余情况下**不要动辄"退回真机"**，也**不要把结论甩给真机**。
  签名与安装由用户完成（**仓库不含任何签名材料**），AI 只做无破坏性动作（push 样本 / 启动 / 读日志）。
- **排查顺序**：代码 / 构建 / 数据 → 模拟器自身状态（跑过重型 GL/GPU 压测后先冷启动）→ 真机（且需授权）。
- **动态通道的重活不得放在 drdynvc 分发线程上**（那条线程承载全部动态通道；帧重活走 `HmrdpSetGfxDataSink`
  交到会话 GFX 工作线程）。线程模型见 [`doc_agent/architecture.md`](doc_agent/architecture.md) §4。
- **改完必须自检**：改 `.ets` 先 `arkts_check`，再 `build_project`；**任务结束前 `build_project` 必须通过**。
- **注释从简**：代码自解释，不写复述类/文档类注释（职责与用法归 `doc_agent/`），不留注释掉的死代码；
  只保留 TODO/FIXME 与非常见的平台/协议/线程规避、性能取舍，且压到一两句。
- **`doc_agent/` 是通用知识库，不是记录**（最容易偏离）：写**结论 + 依据 + 判据 + 约束**，用**现在时、非人称**；
  **不写**"这次/本次/我/发现/修复前/修复后/✅"这类变更与排查语气，**不写机型 / 设备 / 某一次样本的数字**，
  量化只给口径与量级（"整屏帧 ms 级"），不给流水数字。改动属**知识订正**：落进对应主题文件、必要时修正旧结论，
  不要写成按时间累积的日志。**一个事实只有一个家**——改前先在 [`doc_agent/README.md`](doc_agent/README.md) 的
  归属表里找到它**已经**的家，改写那一处，别在新地方另写一份。

## 环境

- `DEVECO_HOME` = DevEco Studio 安装目录（SDK 在 `<DEVECO_HOME>/sdk/default/openharmony`）。
- `targetSdkVersion` = `26.0.0`（能编译哪些 API），`compatibleSdkVersion` = `6.1.0(23)`（能装到多老）：
  **未经确认不要擅自调整**。版本号写法、门槛与告警处理见 [`doc_agent/build-and-verify.md`](doc_agent/build-and-verify.md) §6。
- ABI：`arm64-v8a`（真机，product `default`）、`x86_64`（模拟器，product `emulator`）。
- 宿主为 Windows；原生依赖全部用 CMake + Windows 版 OHOS NDK 构建（**日常构建不需要 WSL 与 Perl**）；
  WSL 只在更换 OpenSSL 版本、重出 `perl Configure` 快照时才用到。

## 常用命令

```bash
devecocli build                                    # product=default → arm64（仅编译）
devecocli build --product emulator                 # 模拟器包（x86_64）
devecocli run --device "<真机序列号>"              # 编译 + 签名 + 安装 + 启动
devecocli run --product emulator --module entry@emulator --device "127.0.0.1:5555"
native/scripts/fetch-sources.ps1                       # 按固定版本拉取三方源码 + 自动打补丁
native/scripts/build-native.ps1 -Arch arm64-v8a        # 编全部原生依赖并同步到模块
native/scripts/install-device.ps1 -Device "<序列号>"   # 安装 + 启动（不接触签名材料）
```

- **签名材料不进仓库**：`signingConfigs` 恒为 `[]`，本机配置放仓库外 `.signing/signing-config.json`，
  由 `hvigorfile.ts` 注入。**切勿**提交 `*.p12`/`*.p7b`/`*.cer`/keystore。
- **改 FreeRDP 一律写成 `patch-steps/` 补丁步骤**（不要在 `third_party/` 里直接改），再
  `fetch-sources.ps1 -Force` + `build-native.ps1 -Only freerdp -Clean`，最后 `build_project`。
  详见 [`doc_agent/native-build.md`](doc_agent/native-build.md)。
- 模拟器优先用于**与硬件加速无关**的功能/逻辑/UI 调试；**Vulkan（硬件加速）只在真机验证**。

## 目录速览

| 路径 | 作用 |
|---|---|
| `entry/src/main/ets/` | ArkUI 界面与业务：`entryability/`、`sessionability/`、`pages/`、`services/`、`moonlight/`、`model/`、`utils/`、`workers/` |
| `entry/src/main/cpp/` | 原生：`binding/`（Node-API 翻译层）+ `core/`（FreeRDP 会话、gdi 帧管线、Vulkan/GLES 呈现器、音频、Moonlight 原生、dev 回放） |
| `entry/src/main/resources/` | 主题色（`base|dark/element/color.json`）与图标 |
| `native/scripts/` | 原生库构建（`build-native.ps1`）与 FreeRDP / Moonlight / zlib 补丁 |
| `entry/libs/<abi>/` | 本地构建的原生库（**gitignore，不入库**） |
| `doc_agent/` | 通用开发知识（见 [`doc_agent/README.md`](doc_agent/README.md)） |

各模块与页面的**详细职责表**见 [`doc_agent/architecture.md`](doc_agent/architecture.md)。

## 改什么读什么

| 要改的东西 | 先读 |
|---|---|
| 整体结构 / 职责表 / 线程与所有权 | [`architecture.md`](doc_agent/architecture.md) |
| 构建 / 运行 / 真机与模拟器 / uitest 技巧 / 最小验证 | [`build-and-verify.md`](doc_agent/build-and-verify.md) |
| 页面 / 状态刷新 / 路由 / 主题 / 沉浸材质机制 | [`arkts-conventions.md`](doc_agent/arkts-conventions.md) |
| 界面外观 / 设计原则 / 抽共享单元 / 设计 token / 组件契约 | [`ui-design.md`](doc_agent/ui-design.md) |
| 两协议共享的会话/连接层（连接模型多态、连接发起、后端、会话外壳、输入内核、会话窗口/输入/工具栏/光标行为、命名边界） | [`session-layer.md`](doc_agent/session-layer.md) |
| RDP 会话 / 输入落地 / 工具栏遥测 | [`rdp-session.md`](doc_agent/rdp-session.md) |
| 剪贴板（手动）/ 文件传输 | [`rdp-clipboard.md`](doc_agent/rdp-clipboard.md) |
| 设置项 / 连接存储 / 密码 / 导入导出 / 生效时机 | [`settings-and-storage.md`](doc_agent/settings-and-storage.md) |
| 原生库构建体系（拉取 / 单一入口 / 依赖布局 / 补丁系统 / OpenSSL CMake） | [`native-build.md`](doc_agent/native-build.md) |
| RDP 原生（FreeRDP 补丁内容 / 音频 / 能力探测 / 编译选项） | [`rdp-native.md`](doc_agent/rdp-native.md) |
| GFX 码流框架 / 会话侧接线 / 回放与参考画面验收（**改前必读**） | [`rdp-gfx.md`](doc_agent/rdp-gfx.md) |
| CPU（gdi）链路的判据、账目口径、门禁、并行形态 | [`rdp-gfx.md`](doc_agent/rdp-gfx.md) §7 |
| 上屏（present）：Vulkan/GLES 后端、脏区上传、零拷贝、超分、swapchain 重建 | [`rdp-present.md`](doc_agent/rdp-present.md) |
| Moonlight 前端（协议建模、存储、列表/编辑页/应用页、设置分栏、图标） | [`moonlight-connection.md`](doc_agent/moonlight-connection.md) |
| Moonlight 原生（客户端库构建/链接、解码、编解码协商、上屏、HTTP、凭证、配对原语） | [`moonlight-native.md`](doc_agent/moonlight-native.md) |
