# 开发知识库（doc_agent）

本目录存放**通用开发知识**：架构、协议/算法约束、平台行为、验证方法。面向所有贡献者与 AI 助手，
与具体开发机、具体设备、具体样本无关。它描述**系统现在是什么样**（结论 + 依据 + 判据 + 约束），
不记录排查过程、日期、机器/设备信息，也不放计划（计划不入库）。

> 入口：仓库根 [`AGENTS.md`](../AGENTS.md)（定位 + 硬规则 + 文档地图 + 环境/命令）。

## 一个事实只有一个家（写文档前先看）

**这是本仓库文档最重要的规则。** 每个事实只在一处展开，其余地方只放一句指针（「见 X」）。
改文档前先在图里找到该事实**已经**的家，改写那一处，不要在新地方另写一份。

| 主题 | 家 |
|---|---|
| 应用结构（Ability / 页面 / 服务 / 原生模块职责表）、数据流、**边界判据（什么必须留在原生）**、原生线程与所有权、拆除顺序、ArkTS 线程模型 | [`architecture.md`](architecture.md) |
| 构建产物与 ABI、签名（材料不入库）、真机与模拟器流程、`uitest`/`hdc` 可靠用法、日志、**改动后必须跑的最小验证**、环境与 SDK 口径 | [`build-and-verify.md`](build-and-verify.md) |
| 严格 ArkTS 规则、ArkUI 状态刷新坑、路由、主题/资源机制、沉浸材质（系统材质）API 与摆放机制 | [`arkts-conventions.md`](arkts-conventions.md) |
| 界面设计语言：设计原则、设计 token、布局模式、组件契约、共享工具、协议差异的呈现约定 | [`ui-design.md`](ui-design.md) |
| **两协议共享的会话 / 连接层**：连接模型多态、连接发起分发、会话后端契约、会话窗口外壳、共享输入内核、会话窗口 / 输入 / 工具栏 / 光标共享行为、通用层命名与边界约定 | [`session-layer.md`](session-layer.md) |
| RDP 会话：连接错误分类、原生会话生命周期、不可见抑制、RDP 输入落地（RDPEI / 高刷 / 直达缩放 / 锁键）、工具栏遥测 | [`rdp-session.md`](rdp-session.md) |
| **剪贴板（手动触发，两协议共享抽象）**与文件传输：共享控制器/桥/本机读写叶子、RDP 线格式、接收 PNG、实现坑 | [`rdp-clipboard.md`](rdp-clipboard.md) |
| 存储分工、密码（ASSET / 系统保险箱）、设置模型（全局默认 + 单连接覆盖）、导入/导出、**设置生效时机** | [`settings-and-storage.md`](settings-and-storage.md) |
| 原生库构建体系：产物为何不入库、单一构建入口、依赖布局与自检、**补丁体系**、OpenSSL CMake、无版本号 SONAME | [`native-build.md`](native-build.md) |
| RDP 原生：FreeRDP 补丁内容与编译选项、音频（原生 sink + 交接环 / ArkTS 渲染器）、**设备能力探测（Capability 模式）** | [`rdp-native.md`](rdp-native.md) |
| GFX 码流框架（两层压缩 / 命令与表面模型 / 会话接线）、**不采用的方向**、dev 回放与**参考画面验收**、**CPU（gdi）链路的成本结构 / 判据与门禁 / 并行形态** | [`rdp-gfx.md`](rdp-gfx.md) |
| 上屏（present）管线：一套呈现器实现、Vulkan/GLES 两个后端、脏区上传、零拷贝桌面缓冲、**超分辨率（Vulkan 专有）**、swapchain 重建 | [`rdp-present.md`](rdp-present.md) |
| Moonlight 连接前端：协议建模、存储布局、连接列表 / 编辑页、设置页分栏、图标约定 | [`moonlight-connection.md`](moonlight-connection.md) |
| Moonlight 主机与应用：主机发现与可达性四态、唤醒（WoL）、应用管理页、连接动作走法 | [`moonlight-apps.md`](moonlight-apps.md) |
| Moonlight 会话：会话窗口 / 后端接线、输入落地、工具栏遥测 | [`moonlight-session.md`](moonlight-session.md) |
| Moonlight 控制 / 配对层（ArkTS）：控制端点、配对时序与身份、证书 pin、标准参数名 | [`moonlight-control.md`](moonlight-control.md) |
| Moonlight 视频：解码（AVCodec）、两条上屏链路、编解码协商、`STREAM_CONFIGURATION` 落点 | [`moonlight-video.md`](moonlight-video.md) |
| Moonlight 原生：客户端库构建与共享库链接、桥接层、本地光标 / 原生触控板 / 音频扩展 | [`moonlight-native.md`](moonlight-native.md) |
| Moonlight 剪贴板（基地版 0x5508 扩展，手动触发）：协议形态、主机能力边界、原生/ArkTS 分工 | [`moonlight-clipboard.md`](moonlight-clipboard.md) |

**跨主题、只有一个家的事实**（别在别处重写）：

- **硬件加速开关只选上屏后端，解码始终是 gdi** → [`rdp-present.md`](rdp-present.md) §1（设置意义见 [`settings-and-storage.md`](settings-and-storage.md)）。
- **自研 GPU 解码/合成引擎不采用** → [`rdp-gfx.md`](rdp-gfx.md) 的「不采用的方向」。
- **帧重活不得放在动态通道分发线程上** → [`architecture.md`](architecture.md) 的线程模型。
- **与设备相关的能力用 `Capability{supported, reason}` + 置灰说明** → [`rdp-native.md`](rdp-native.md) 的能力探测。
- **剪贴板只手动同步、不做自动监听** → [`rdp-clipboard.md`](rdp-clipboard.md)。
- **剪贴板抽象（`ClipboardController` + `ClipboardBridge` + `LocalPasteboard`）两协议共用**：RDP 全能力（文本/HTML/图片/文件），Moonlight 仅文本 + 图片 → [`rdp-clipboard.md`](rdp-clipboard.md) §0；Moonlight 协议见 [`moonlight-clipboard.md`](moonlight-clipboard.md)。
- **Moonlight 默认解码直出 surface、不经呈现器；只有超分链路复用 RDP 的 Vulkan 呈现器** → [`moonlight-video.md`](moonlight-video.md) §2/§5。
- **Moonlight 是进程级单会话**（`moonlight-common-c` 一次只允许一个连接）→ [`moonlight-native.md`](moonlight-native.md) §2。
- **Moonlight 主机身份 = serverinfo `uniqueid`（去重）/ pin 按 host 归一化** → [`moonlight-control.md`](moonlight-control.md) §2。

## 阅读顺序

| 目标 | 先读 |
|---|---|
| 首次上手 / 搭环境 / 跑起来 | [`build-and-verify.md`](build-and-verify.md) |
| 了解整体结构 / 线程模型 | [`architecture.md`](architecture.md) |
| 改 ArkTS / ArkUI / 页面 | [`arkts-conventions.md`](arkts-conventions.md) + [`architecture.md`](architecture.md) |
| 改界面外观 / 抽共享 UI 组件 | [`ui-design.md`](ui-design.md) |
| 改两协议通用层（连接模型 / 后端 / 会话外壳 / 输入内核 / 会话窗口行为 / 工具栏 / 光标） | [`session-layer.md`](session-layer.md) |
| 改 RDP 会话 / 输入落地 / 工具栏遥测 | [`rdp-session.md`](rdp-session.md) |
| 改剪贴板 / 文件传输 | [`rdp-clipboard.md`](rdp-clipboard.md) |
| 改设置 / 连接存储 / 密码 / 导入导出 | [`settings-and-storage.md`](settings-and-storage.md) |
| 改原生库构建 / 补丁体系（FreeRDP / OpenSSL / zlib / moonlight） | [`native-build.md`](native-build.md) |
| 改 RDP 原生（补丁内容 / 音频 / 能力） | [`rdp-native.md`](rdp-native.md) |
| 改 GFX / 解码 / 回放 | [`rdp-gfx.md`](rdp-gfx.md)（**CPU 链路先读 §7**） |
| 改上屏（present）/ 超分 | [`rdp-present.md`](rdp-present.md) |
| 改 Moonlight 前端（列表 / 编辑页 / 设置分栏 / 图标） | [`moonlight-connection.md`](moonlight-connection.md) |
| 改 Moonlight 主机与应用（发现 / 运行态 / 唤醒 / 应用页） | [`moonlight-apps.md`](moonlight-apps.md) |
| 改 Moonlight 会话 / 遥测 | [`moonlight-session.md`](moonlight-session.md) |
| 改 Moonlight 控制 / 配对 | [`moonlight-control.md`](moonlight-control.md) |
| 改 Moonlight 视频（解码 / 上屏 / 协商） | [`moonlight-video.md`](moonlight-video.md) |
| 改 Moonlight 原生（客户端库 / 桥接 / 光标 / 触控板 / 音频） | [`moonlight-native.md`](moonlight-native.md) |
| 改 Moonlight 剪贴板 | [`moonlight-clipboard.md`](moonlight-clipboard.md) |

## 写作约定

- **只写结论、依据、判据与约束。** 一次性实验、失败尝试、排查过程不保留。某方向被否证且看起来仍然诱人时，
  以「该方向不成立 + 原因」的形式写进对应主题的「不采用」处，避免后人复活它。
- **优先给量级、比例与判据**，而非不可复现的逐帧数字；数字只在作为阈值或门槛时保留。
- **注明适用面**（如「仅真机」「模拟器不参与」「仅在已打补丁的构建上」），避免后人误用。
- **改了被代码引用的章节要同步引用**：代码/脚本注释里有 `doc_agent/<file>.md §N` 形式的锚点
  （多为 `rdp-gfx.md §7.x`、`architecture.md §4.x`、`rdp-native.md §3`），移动或重排章节时一并更新。
