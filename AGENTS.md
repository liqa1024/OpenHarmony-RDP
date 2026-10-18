# AGENTS.md

面向 **鸿蒙 PC（2in1）** 的 RDP / Moonlight 云串流客户端（**HmRdp**，`com.lixa.hmrdp`）。
RDP 用 FreeRDP，Moonlight 用 `moonlight-common-c`；共用连接列表、会话外壳与输入分流。
界面 ArkTS/ArkUI，原生经 Node-API 桥接；RDP 画面由 gdi 出，Vulkan 优先 / GLES 兜底；Moonlight 默认直出 surface。
用户文档见 [`README.md`](README.md)，通用开发知识与文档归属见 [`doc_agent/README.md`](doc_agent/README.md)。

本文介绍重点约束和高频内容，具体细节参见 [`doc_agent/`](doc_agent/README.md)。

## 红线

- **git 不写**：禁止任何 git 写操作。理解代码和项目结构优先直接读工作区文件；`status`/`log`/`show`/`diff`/`blame`
  只用于查状态、历史、差异和追溯，不要拿它代替直接读代码。恢复文件一般直接编辑；内容较多时，
  可以让用户授权本次恢复，或交给用户操作。
- **签名材料不进仓库**：AI 不接触 `*.p12`/`*.p7b`/`*.cer`/keystore 或签名路径；
  细节见 [`build.md`](doc_agent/build.md) §2。
- **`targetSdkVersion` / `compatibleSdkVersion` 未经确认不要调整**。

## 理解、授权与行动边界

**先斩后奏不是效率，是越权。** 用户的话不只是待核实的背景信息，更是目标、约束、授权范围和验收标准的来源。
调研可以比用户更懂事实，但不能因此替用户改写他想做什么；证据只授权你修正事实，不授权你补全用户没说的指代、意图或授权。

### 1. 先理解，再行动

- 用户提问时先回答或澄清；不要把问题自动当成执行指令。
- 指代、范围或验收可能不唯一时，先复述你所指的对象并确认；不得用历史记录、其他文件、其他会话或推测自行补全。
- 引用历史、其他文件或其他会话时，先说明来源；它们不是当前指令，也不是当前授权。
- 理解差异会影响行动、范围或验收时，先确认。不能把“这很明显”“用户说得不准”当作跳过确认的理由；用户表述正常时，误解仍是 AI 的责任。

### 2. 行动与授权

- 只读调研（读代码、文档、日志、状态）可以直接做；能只读不写，能局部不全局，能不动外部状态就不动。
- 用户明确要求某个具体动作时，按该动作和已知路径执行；不自动换方法、换设备或扩大范围。
- 有外部副作用、不可逆或会改变用户可见状态 / 共享环境的动作，先说明并等明确同意。任务内的构建、静态检查和文件编辑按任务授权执行；任务外的状态变更不是“顺手做一下”。
- 一次授权只覆盖明确说出的动作、对象和范围；不延伸到替代方案、其他对象或更大范围。历史成功和其他任务的做法都不是当前授权。
- 只有用户明确说“直接做 / 不用问 / 按你的判断”时，才可在其指定范围内自行决定。

### 3. 出现以下任一情况，先停

- 查到的东西与用户的原话、原方案或明确要求不一致；
- 准备改变假设、范围、计划、验收标准，或执行有外部副作用的动作；
- 发现自己正在用“用户不懂 / 说得不准 / 先做完再说”说服自己；
- 失败后准备换方法、换路径、换设备或扩大范围。

停下来的动作只有一个：用一句话说清“用户原话—你查到 / 理解什么—你准备做什么（副作用 / 范围）”。必须先说，不能留到最终总结，不能用“我稍后说明”代替。

然后分流：用户明确说过“不要 / 必须”的事，以及任何有外部副作用、不可逆或会改变用户可见状态的动作，先等确认；其余内部推进可以继续，但偏离必须已经说出口。需要确认的动作，说出口不等于授权，必须等明确同意。不要用“少打扰”“先交付”当省略这句话的理由——判断标准是“是否改变用户的目标、范围、授权或外部状态”，不是“是否打扰用户”。

### 4. 失败与越界

- 失败只授权汇报与诊断，不授权换方案、扩范围或连续重试；要换路径，先说明原因和影响并确认。
- 一次只改一个变量；不要把“排查 / 验证”当作绕过授权的理由。
- 已越界时立即停手，说明已经做了什么、影响范围、是否可恢复；不要继续执行或自行“补救”扩大影响。恢复动作同样先确认。

事实可以让证据说话；但证据只授权你修正事实，不授权你替用户决定目标。未验证、失败、假设不能混在“已完成”里。**用户看不到的修正，等于没发生。**

### 5. 用户问题按类处理

- 用户报告的是**样本**，不是唯一缺陷。先判断它属于哪一类（同一组件、同一反馈通道、同一交互模式、同一 API 或生命周期），做**只读审计**确认是否存在同类问题；不得只修截图或复现路径上的那一处而不看同类。
- **同类扩大修复不是默认授权**：审计确认存在同类问题、需要扩大修复范围时，先按 §3 停下说明：问题类别、已确认位置、拟修改范围、方案与风险，等用户明确同意后再改。
- 不得以“同类”为由自行批量修改，也不得只修报告点而隐瞒审计发现的同类问题；未修项和待确认项要列明。
- 修复完成后说明：问题类别、已修位置、同类审计结果、未修 / 待确认项。

## 注意事项

- **构建与自检**：改 `.ets` / `.cpp` 后按 [`build.md`](doc_agent/build.md) §4 的构建门禁走；
  `build_project` 不通过不算完成。
- **模拟器与真机**：先弄清模拟器和实机的验证流程与区别，再决定用哪种；占用/关闭与真机例外规则见
  [`testing.md`](doc_agent/testing.md)。
- **UI/视觉判断**：归用户；AI 只做构建、日志和可判定的自动检查，不默认重复验收，也不因复现不了就否定用户的观察。
- **提示与反馈**：新增 Toast / Notice / Confirm / 内联提示 / 加载态前先读
  [`ui-feedback.md`](doc_agent/ui-feedback.md)；同一事件只走一个通道，关键错误不用 Toast。
- **Vulkan/硬件加速**：按 [`testing.md`](doc_agent/testing.md) 在真机验证；模拟器不作为依据。
- **调试**：查日志、驱动 UI 前先读 [`testing.md`](doc_agent/testing.md)，
  按文档里的可靠用法做，不要凭记忆盲点；多设备和日志参数以文档为准。
- **环境与产物**：确认 `DEVECO_HOME`、SDK 版本、ABI/product 映射；详见
  [`build.md`](doc_agent/build.md) §1/§5。版本未经确认不要改。
- **原生构建**：改原生依赖前先读 [`native-build.md`](doc_agent/native-build.md) 的补丁/构建流程；
  RDP 补丁内容见 [`rdp-native.md`](doc_agent/rdp-native.md) §3。

## 约定

- 最终答复/总结用中文（代码、命令、标识符除外）。
- 注释只留 TODO/FIXME 和非常见的平台/协议/线程规避、性能取舍；不复述代码，不留死注释。

## 文档索引

| 文档 | 内容 |
|---|---|
| [`doc_agent/README.md`](doc_agent/README.md) | 归属表、阅读顺序、写作约定 |
| [`architecture.md`](doc_agent/architecture.md) | 整体结构、职责表、线程与所有权 |
| [`build.md`](doc_agent/build.md) | 构建、签名、构建/安装命令、最小构建门禁、环境与 SDK 口径 |
| [`testing.md`](doc_agent/testing.md) | 测试与验证：AI 自测、默认流程、用户 UI 验收、AI 实机例外、用户操作 + hilog、日志、UI 驱动、交接 |
| [`arkts-conventions.md`](doc_agent/arkts-conventions.md) | 严格 ArkTS 规则、ArkUI 状态刷新、路由、资源与主题、与原生交互 |
| [`ui-framework.md`](doc_agent/ui-framework.md) | 界面结构、组件、交互触发、组件契约（不含配色/材质/尺寸取值） |
| [`ui-visual.md`](doc_agent/ui-visual.md) | 界面视觉：颜色、主题、表面、材质、字号、间距、圆角、阴影、动效、图标画法 |
| [`ui-feedback.md`](doc_agent/ui-feedback.md) | 反馈与提示：系统模型、通道选择、优先级、宿主与生命周期、文案、时长、无障碍 |
| [`session-layer.md`](doc_agent/session-layer.md) | 两协议共享的会话/连接层：连接模型、连接发起、后端、会话外壳、输入内核、窗口/工具栏/光标行为、命名边界 |
| [`rdp-session.md`](doc_agent/rdp-session.md) | RDP 会话、输入落地、工具栏遥测 |
| [`clipboard.md`](doc_agent/clipboard.md) | 剪贴板共享抽象与手动同步语义 |
| [`rdp-clipboard.md`](doc_agent/rdp-clipboard.md) | RDP 剪贴板线格式、文件传输 |
| [`settings-and-storage.md`](doc_agent/settings-and-storage.md) | 设置项、连接存储、密码、导入导出、生效时机 |
| [`native-build.md`](doc_agent/native-build.md) | 原生库构建体系：拉取、单一入口、依赖布局、补丁系统、OpenSSL CMake |
| [`rdp-native.md`](doc_agent/rdp-native.md) | RDP 原生：FreeRDP 补丁内容、音频 sink 侧、编译选项 |
| [`audio.md`](doc_agent/audio.md) | 音频链路：共享交接环、ArkTS 渲染器、原生 OHAudio、音频开关 |
| [`rdp-gfx.md`](doc_agent/rdp-gfx.md) | GFX 码流框架、会话侧接线、回放与参考画面验收；CPU（gdi）链路的判据、账目口径、门禁、并行形态（§7） |
| [`present.md`](doc_agent/present.md) | 上屏共享管线：Vulkan/GLES 后端、帧到屏、swapchain 重建、超分与视频导入、GPU 账目、呈现/超分能力判定 |
| [`rdp-present.md`](doc_agent/rdp-present.md) | RDP 上屏接线：脏区上传、零拷贝、合并上屏、RDP 侧账目 |
| [`moonlight-connection.md`](doc_agent/moonlight-connection.md) | Moonlight 连接前端：协议建模、存储、列表/编辑页、设置分栏、图标语义 |
| [`moonlight-apps.md`](doc_agent/moonlight-apps.md) | Moonlight 主机与应用：发现、运行态、唤醒、应用管理页、连接动作走法 |
| [`moonlight-session.md`](doc_agent/moonlight-session.md) | Moonlight 会话：窗口接线、工具栏遥测 |
| [`moonlight-control.md`](doc_agent/moonlight-control.md) | Moonlight 控制/配对层：控制端点、配对时序与身份、证书 pin |
| [`moonlight-video.md`](doc_agent/moonlight-video.md) | Moonlight 视频：解码、上屏、编解码协商 |
| [`moonlight-native.md`](doc_agent/moonlight-native.md) | Moonlight 原生：客户端库构建/链接、桥接、光标/触控板/音频扩展 |
| [`moonlight-clipboard.md`](doc_agent/moonlight-clipboard.md) | Moonlight 剪贴板：协议形态、主机能力边界、原生/ArkTS 分工 |

完整归属与阅读顺序见 [`doc_agent/README.md`](doc_agent/README.md)。
