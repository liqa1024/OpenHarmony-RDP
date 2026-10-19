# Moonlight 会话（窗口接线 / 工具栏遥测）

本文件是 Moonlight 路的会话窗口接线与工具栏遥测的家——与 [`rdp-session.md`](rdp-session.md) 平行。
两协议共享的会话模型 / 状态机 / 窗口与全屏 / 自动隐藏 / 输入策略 / 工具栏外壳 / 光标安装器见
[`session-layer.md`](session-layer.md)；连接与编辑页见 [`moonlight-connection.md`](moonlight-connection.md)；
主机与应用（运行态 / 关闭应用）见 [`moonlight-apps.md`](moonlight-apps.md)；控制/配对见
[`moonlight-control.md`](moonlight-control.md)；解码与解码格式见 [`moonlight-video.md`](moonlight-video.md)；
原生桥接见 [`moonlight-native.md`](moonlight-native.md)。

## 1. 会话窗口与后端

- `pages/MoonlightSessionPage.ets`（XComponent 先绑 surface，再 `SessionManager.beginStream` 启动）、
  `services/MoonlightSessionBackend.ets`（`prepare()` 跑控制层预检并装配 `moonlightConfig`，`start()` 交给
  原生）、`services/SessionBackendFactory.ets` 与 `utils/MoonlightKeyMapper.ets`。
- `SessionAbility` 按 `SessionBackend.pageRoute()` 路由到该页（**不判断协议**），窗口外壳复用共享的
  `services/SessionShellController`（见 [`session-layer.md`](session-layer.md) §4）。
- 窗口在**预检通过后**打开、绑好 surface 再启动（`requiresWindowAtStart`，因为 `LiStartConnection` 需要渲染
  目标）。连接中的状态卡带约 1000ms 显示阈值，避免"多闪一下"。
- 工具栏与 RDP 同形：标题用共享的 `SessionTitle`（名称 + 分辨率 + 括号里的**会话标签**——超分与协商解码格式
  合并到同一个括号、逗号分隔，如 `(FSR 1.5x, HEVC)`），复用同一套
  chrome 与自动隐藏判定（`components/SessionChrome`、`services/ToolbarAutoHide`），支持全屏 / 最小化。
- **剪贴板入口**：主机广告剪贴板能力位且已连接时，工具栏插入共享的 `ClipboardButtons`（复制 / 系统 `PasteButton` 粘贴）；
  非基地版主机（无能力位）不显示入口。协议与语义见 [`moonlight-clipboard.md`](moonlight-clipboard.md)。
- **停止会话会释放原生窗口**，所以窗口内「重试」先重新绑定仍存活的 XComponent surface 再重启（此时会话必然
  已不在活动态，原生允许再次绑定）。
- 输入落地：共享策略在 [`session-layer.md`](session-layer.md) §8，原生出口见
  [`moonlight-native.md`](moonlight-native.md) §2。
- **音频启动按模式分流**：连接后 `startAudio()` 读 `mlLowLatencyAudio`——为真时原生 OHAudio 接管播放，本页
  **不**启动 ArkTS 音频 worker 的渲染器（否则双份播放）。机制见 [`moonlight-native.md`](moonlight-native.md) §2.3。

## 2. 开发者工具栏遥测

开发者选项开启且会话已连接时另起第二行，行由共享的 `TelemetryRow` 渲染。字段固定为
**网络 / 主机 / 本机 / GPU / FPS / 码率**，全部定宽，口径如下。

- **网络** = `LiGetEstimatedRttInfo` 的 RTT（着色阈值同 RDP）；第二行给**抖动**（第二返回值）与**视频丢包率**
  （`LiGetRTPVideoStats` 的 FEC 恢复失败 + 乱序 + 损坏 ÷ 总包）。
- **主机** = 主机帧处理耗时（Sunshine 在帧头携带的 `frameHostProcessingLatency`，单位 1/10ms，取窗口内均值）；
  主机不提供该值（GFE / 重复帧）时显示 `主机 --`；着色与「本机」同一套阈值。
- **本机** = 本机解码往返（该帧从提交解码到解码出帧的墙钟，按 `pts` 配对后取窗口均值）——与 RDP 的「本机」
  同为**每帧本机耗时**、子项不同（RDP 是四相之和，此处是单相）；该读数是**上界**（含输入排队与输出缓冲
  等待，见 [`moonlight-video.md`](moonlight-video.md) §1），故**着色阈值按目标帧周期缩放**：超 2×帧周期转黄、
  超 3×帧周期转红——只在明显超出解码帧预算（真过载）时才变色，不被正常排队误触发；第二行给在途待解码
  帧数（`LiGetPendingVideoFrames`）与**总丢弃率**（输入预算丢弃 + 送显闸门丢弃 + 交接替换，见
  [`moonlight-video.md`](moonlight-video.md) §1）。
- **GPU** = 超分链路上**每帧**的呈现器工时（convert / 超分 / letterbox 三段之和）；直出链路没有呈现器，
  显示 `GPU --`。第二行给三段拆分与**等显示**（`vkAcquireNextImageKHR` 的阻塞）。这三段与 RDP 的
  `gpuCopyUs`/`gpuSrUs`/`gpuBlitUs` 同源，只是视频路径的 `copy` 段是 YUV→RGB 的 convert pass。
- **过载提示**：颜色只说「不好」，说不出「改什么」，所以某一侧**连续两个采样窗口**都越过阈值（本机 = 3×
  帧周期，取 `max(解码往返, GPU 每帧)`）时弹一次提示。文案**一行一动作**（`本机处理跟不上，建议降低分辨率
  或调整超分` / `主机编码跟不上，建议降低分辨率并开启超分`）：降像素是两侧共有的最强杠杆（本机少解、
  主机少编），帧率在连接时已按面板/解码器上限钳制、只在还不够时才是下一档；展开的理由与更多手段留给
  第二行。两侧各自计数（本机过载不吃掉主机的提示），且同一侧在读数回落到阈值以下之前不再重复——
  单个窗口的抖动（场景切换、重同步）不触发，也不会重复刷屏（`services/OverloadWatch`）。提示不依赖
  开发者模式（依赖它的是数字那一行本身）。
- **刷新率提示**：`refreshDroppedFrames` 是遥测里**唯一归因于屏幕**的丢弃数——只累计「丢弃发生时面板档位
  低于协商帧率」的那部分（判据 `DisplayBelowFrameRate()`，三个丢弃点各自判一次，链无关；见
  [`moonlight-video.md`](moonlight-video.md) §1）。所以它的占比（÷ 送显 + 它自己）连续两个采样窗口 >10%
  就说明**面板刷新率低于流帧率**。这是应用侧改不动、只能由用户在系统设置固定高刷的情形（程序侧没有可用
  接口），故给一行一动作的会话内 Toast（`Long`，自动消失）`屏幕刷新率不足导致丢帧，可在系统设置固定高刷新率`
  ——**只在请求帧率 >60 的会话**提示（低帧率会话本就不受低档位影响），且**每次会话最多一次**
  （`services/RefreshHintWatch`，随连接重置），诊断读数仍留给第二行。窗口取 1s 采样定时器。
  **两条文案的降像素思路相同**：低分辨率送流 + 本机放大，同一处降低**两侧**的像素工作量——主机少编、
  本机少解——代价是本机多一趟上采样（链路见 [`moonlight-video.md`](moonlight-video.md) §2），
  所以它同时是**主机过载**的出路，不只是本机的。
- **FPS** = 当前**实际呈现帧**率 / 请求帧率（surface 模式计解码器取得送显槽位的帧，超分模式计呈现器
  提交成功的帧；不是解码器输出的总帧数）；**码率** = 当前视频载荷码率（Σ 帧字节 ÷ 窗口）/ 配置码率。
  两处都用 `当前/目标` 写法，目标是发起时请求的 `fps` / `bitrateKbps`；码率单位只出现一次、两数共用
  （`数字/数字 单位`，≥1Mbps 用 `Mbps` 否则 `kbps`）。
- **不做** CPU（全硬件解码）；音频已接线但**不进遥测**（无音频丢帧指标）。

原生只产**累计计数与瞬时读数**，速率与每帧均值在 ArkTS（`services/MoonlightTelemetry`）聚合；
采样源与累加点见 [`moonlight-native.md`](moonlight-native.md) §2。
