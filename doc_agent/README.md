# 开发知识库（doc_agent）

本目录存放**通用开发知识**：架构、协议/算法约束、平台行为、验证方法。面向所有贡献者与 AI 助手，
与具体开发机、具体设备、具体样本无关。

> 入口：仓库根目录的 [`AGENTS.md`](../AGENTS.md)（概览 + 硬规则 + 文档索引）。
> 本目录只写**长期结论**：结论 + 依据（协议 / FreeRDP 源码 / 实测）+ 约束（改谁会影响谁），
> 不记录排查过程、日期与机器/设备信息。

## 阅读顺序

| 目标 | 先读 |
|---|---|
| 首次上手 / 搭环境 / 跑起来 | [`build-and-verify.md`](build-and-verify.md) |
| 改原生库（FreeRDP / OpenSSL / 音频） | [`native-libraries.md`](native-libraries.md) |
| 改 GFX / Progressive / GPU 引擎 | [`gfx-engine.md`](gfx-engine.md)（**先读「必须保留的语义」**） |
| 改上屏（present）管线 | [`present-pipeline.md`](present-pipeline.md) |
| 改 CPU（gdi）链路的性能 / 解码线程数 | [`cpu-accel-plan.md`](cpu-accel-plan.md)（**接手文档**：口径 + 优化清单 + 里程碑） |
| 做 GPU 硬件加速（tile 解码 + 合成的 GPU 化） | [`gpu-accel-plan.md`](gpu-accel-plan.md)（**计划**，含里程碑与出口） |
| 查历史依据（**old 弃用**，不要从这里接手） | [`cpu-path_old.md`](cpu-path_old.md)、[`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md) |
| 改 ArkTS / ArkUI / 页面 | [`arkts-conventions.md`](arkts-conventions.md) + [`architecture.md`](architecture.md) |
| 改会话窗口 / 输入 / 工具栏 / 遥测 | [`session-and-input.md`](session-and-input.md) |
| 改设置 / 连接存储 / 密码 / 导入导出 | [`settings-and-storage.md`](settings-and-storage.md) |

## 文档清单

- [`build-and-verify.md`](build-and-verify.md) —— 构建产物（product/ABI）、签名（材料不入库）、
  真机与模拟器流程、`uitest`/`hdc` 的可靠用法、日志读取、**改动后必须跑的最小验证**。
- [`architecture.md`](architecture.md) —— 应用结构：Ability / 页面 / 服务 / 原生模块职责表，连接与会话的数据流。
- [`arkts-conventions.md`](arkts-conventions.md) —— 严格 ArkTS 规则、ArkUI 状态/`@Builder`/`ForEach`/路由/主题约定。
- [`session-and-input.md`](session-and-input.md) —— 会话窗口模型、鼠标/键盘/触屏/触控板输入映射、工具栏与状态遥测、
  光标同步、剪贴板（手动触发）。
- [`settings-and-storage.md`](settings-and-storage.md) —— 设置与连接的存储模型、密码（ASSET）、导入/导出、
  **设置何时生效**与解析优先级。
- [`native-libraries.md`](native-libraries.md) —— FreeRDP/OpenSSL/zlib 的源码构建与补丁、无版本号 SONAME、
  为什么 `entry/libs/<abi>/` 不入库、音频（OHAudio）与能力探测（Capability）模式。
- [`gfx-engine.md`](gfx-engine.md) —— GFX 码流与 GPU 引擎：管线框架、**必须保留的协议/算法语义**、
  性能规则、回放验证回路与**参考画面（golden reference）验收**、待办。
- [`present-pipeline.md`](present-pipeline.md) —— 上屏（present）管线：统一的呈现器实现、帧槽与
  设备侧握手、picture ping-pong、CPU/GPU 耗时对比与后续工作清单。
- [`cpu-accel-plan.md`](cpu-accel-plan.md) —— **CPU（gdi）链路优化计划（接手文档）**：判据、每帧成本结构与
  账目口径、已定型项与约束、`dec` 之内的相位实测占比与**优化清单 C1–C3（含各自的门禁与出口）**、
  并行能效曲线、量测纪律与被否证的假设。
- [`gpu-accel-plan.md`](gpu-accel-plan.md) —— **GPU 硬件加速计划**：把 tile 解码 + 合成做成
  一个"载荷进、像素出"的 GPU 阶段（相位适配性与并行度分析、交接成本模型、M0–M4 里程碑与出口、
  开关与回退口径）。
- **（old 弃用，只作历史依据）**：
  [`cpu-path_old.md`](cpu-path_old.md) —— 旧 CPU 链路文档（与 `cpu-accel-plan.md` 同章节号，内容已被其取代）；
  [`gfx-progressive-kernel_old.md`](gfx-progressive-kernel_old.md) —— 旧 GPU kernel 清单
  （RLGR producer/consumer）；两者其中的**约束与实测仍被引用**，但**不要作为接手入口**。

## 维护约定

- 新增结论先判断主题归属，写进对应文件；**不要**新建按时间顺序的排查记录。
- 只保留结论、依据与约束。一次性实验、失败尝试不保留；若某方案被否证且看起来仍然诱人，
  以「该方向不成立 + 原因」的形式写进对应主题。
- 涉及平台行为的条目注明**适用面**（如"仅真机"、"模拟器不参与"），避免后人误用。
- 优先给**量级、比例与判据**，而非不可复现的逐帧数字；数字只在作为阈值或门槛时保留。
