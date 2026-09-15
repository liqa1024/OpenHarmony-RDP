# 开发知识库（doc_agent）

本目录存放**通用开发知识**：架构、协议/算法约束、平台踩坑、验证方法。面向所有贡献者与 AI 助手，
与具体开发机、具体设备无关。

> 入口：仓库根目录的 [`AGENTS.md`](../AGENTS.md)（概览 + 硬规则 + 文档索引）。
> 本目录的文档是**长期知识**，不记录排查过程、不记录日期、不写具体机器/设备信息。

## 阅读顺序

| 目标 | 先读 |
|---|---|
| 首次上手 / 搭环境 / 跑起来 | [`build-and-verify.md`](build-and-verify.md) |
| 改原生库（FreeRDP / OpenSSL / 音频） | [`native-libraries.md`](native-libraries.md) |
| 改 GFX / Progressive / GPU 引擎 | [`gfx-engine.md`](gfx-engine.md)（**先读"必须保留的语义"**） |
| 查两份录像的逐像素分叉 / 跑对拍 A/B | [`gfx-vulkan-correctness.md`](gfx-vulkan-correctness.md) |
| 改 RLGR 解码 kernel 的并行化 | [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)（**后续工作清单**） |
| 改 ArkTS / ArkUI / 页面 | [`arkts-conventions.md`](arkts-conventions.md) + [`architecture.md`](architecture.md) |
| 改会话窗口 / 输入 / 工具栏 / 遥测 | [`session-and-input.md`](session-and-input.md) |
| 改设置 / 连接存储 / 密码 / 导入导出 | [`settings-and-storage.md`](settings-and-storage.md) |

## 文档清单

- [`build-and-verify.md`](build-and-verify.md) —— 构建产物（product/ABI）、签名（不入库）、真机与模拟器流程、
  `uitest`/`hdc` 的可靠用法、日志读取、**改动后必须跑的最小验证**。
- [`architecture.md`](architecture.md) —— 应用结构：Ability / 页面 / 服务 / 原生模块职责表，连接与会话的数据流。
- [`arkts-conventions.md`](arkts-conventions.md) —— 严格 ArkTS 规则、ArkUI 状态/`@Builder`/`ForEach`/路由/主题约定。
- [`session-and-input.md`](session-and-input.md) —— 会话窗口模型、鼠标/键盘/触屏/触控板输入映射、工具栏与状态遥测、
  光标同步、剪贴板（手动触发）。
- [`settings-and-storage.md`](settings-and-storage.md) —— 设置与连接的存储模型、密码（ASSET）、导入/导出、
  **设置何时生效**与解析优先级。
- [`native-libraries.md`](native-libraries.md) —— FreeRDP/OpenSSL/zlib 的源码构建与补丁流程、无版本号 SONAME、
  为什么 `entry/libs/<abi>/` 不入库、音频（OHAudio）与能力探测（Capability）模式。
- [`gfx-engine.md`](gfx-engine.md) —— GFX 码流与 GPU 引擎：架构、**必须保留的协议/算法语义**、性能规则、
  真机专属口径、**回放验证回路与 `bad=0` 验收**、待办。
- [`gfx-vulkan-correctness.md`](gfx-vulkan-correctness.md) —— 引擎 vs gdi 逐像素对拍这条线：验收口径、
  两份录像、**已修的分叉（compose 派发超限、UPGRADE 拒收语义、参考实现未初始化状态等）+ 依据**、
  视频录像的**残留与下一步**、分级 A/B 工具与开发开关。
- [`gfx-progressive-kernel.md`](gfx-progressive-kernel.md) —— **后续工作清单**：RLGR 解码 kernel 的
  producer/consumer 并行化（依据、实现要点、已踩的坑、已否决方案、清理项）。

## 维护约定

- 新增"踩坑结论"时，先判断它属于哪个主题并写进对应文件；**不要**再新建一份按时间顺序的排查记录。
- 只写**结论 + 依据（谁规定的：协议/FreeRDP 源码/实测）+ 约束（改谁会影响谁）**；排查过程的失败尝试不必保留。
- 涉及平台行为的条目请注明**适用面**（例如"仅真机"、"模拟器不参与"），避免后人误用。
