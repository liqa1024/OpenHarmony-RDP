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
| 改 GFX / 解码 / 回放 | [`gfx-engine.md`](gfx-engine.md) |
| 改上屏（present）管线 | [`present-pipeline.md`](present-pipeline.md) |
| 改 CPU（gdi）链路的解码 / 成本 / 量测口径 | [`gfx-engine.md`](gfx-engine.md) §8（**先读这一节**） |
| 做 CPU 解码的并行 / 执行器 / 任务划分 / 内存归属 | [`cpu-accel-plan.md`](cpu-accel-plan.md)（并行范围、执行器与宽度、任务划分、内存归属、合成归属） |
| 改 ArkTS / ArkUI / 页面 | [`arkts-conventions.md`](arkts-conventions.md) + [`architecture.md`](architecture.md) |
| 改会话窗口 / 输入 / 工具栏 / 遥测 | [`session-and-input.md`](session-and-input.md) |
| 改通道线程 / 音频不被帧挡住 | [`architecture.md`](architecture.md) §4（线程模型） |
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
- [`gfx-engine.md`](gfx-engine.md) —— GFX 码流框架：管线与命令模型、会话侧接线、
  回放验证回路与**参考画面（golden reference）验收**、**CPU（gdi）链路的成本结构与账目口径 /
  判据与门禁（§8）**、待办。
- [`present-pipeline.md`](present-pipeline.md) —— 上屏（present）管线：一套呈现器实现、Vulkan/GLES 两个
  后端、脏区上传与零拷贝桌面缓冲的定型约束、present 账目与探针。
- [`cpu-accel-plan.md`](cpu-accel-plan.md) —— **CPU（gdi）链路的并行方案（现状）**：并行范围与线程归属
  （唯一并行段 = region 的 tile 解码 + worker 侧合成拷贝；调用线程也跑一个 chunk）、唯一执行器
  （ffrt 并发队列，宽度 = 在线核数、队列并发度 = 宽度 − 1）、任务划分（home + 段尾块偷取）、
  内存归属（per-chunk scratch 槽、surface 级 tile arena）、合成归属（worker 直写 + clip 哈希去重）。
  单核部分的知识在 [`gfx-engine.md`](gfx-engine.md) §8。

## 维护约定

- 新增结论先判断主题归属，写进对应文件；**不要**新建按时间顺序的排查记录。
- 只保留结论、依据与约束。一次性实验、失败尝试不保留；若某方案被否证且看起来仍然诱人，
  以「该方向不成立 + 原因」的形式写进对应主题。
- 涉及平台行为的条目注明**适用面**（如"仅真机"、"模拟器不参与"），避免后人误用。
- 优先给**量级、比例与判据**，而非不可复现的逐帧数字；数字只在作为阈值或门槛时保留。
