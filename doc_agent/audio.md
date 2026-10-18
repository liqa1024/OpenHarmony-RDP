# 音频播放链路（两协议共享）

本文件是**两协议共享的音频播放链路**的家：有界 PCM 交接环（`core/hmrdp_audio.*`）、ArkTS 渲染器与原生
OHAudio 两条消费路径、缓冲策略与格式变化。RDP 侧的 rdpsnd sink 与 FreeRDP 补丁见
[`rdp-native.md`](rdp-native.md) §5；Moonlight 侧的 Opus 解码桥与低延迟模式见
[`moonlight-native.md`](moonlight-native.md) §2.3；库侧解码线程与 ArkTS 音频 worker 的归属见
[`architecture.md`](architecture.md) §4.2/§4.4。

## 1. 边界：强制原生的只有生产者与交接环

- 两协议都把**解码后的 PCM** 写进同一个**有界 PCM 交接环**（`AudioSink`，`core/hmrdp_audio.*`）：
  RDP 由 FreeRDP 的 rdpsnd 后端写入，Moonlight 由 libopus 解码桥写入。生产者都跑在库自有的解码线程上，
  且必须**同步返回**渲染延迟（RDP 的 sink 还要把延迟回填 Wave Confirm）——与文件上传 serve 同一类
  同步回调约束（[`architecture.md`](architecture.md) §3.1 判据 2），所以环与它的写入适配器留在原生。
- 环的水位用**播放时长（ms）**表达，与格式无关（按 `rate × channels × 2` 折算）。

## 2. 播放 API 归 ArkTS（默认路径）

- `OH_AudioRenderer`（OHAudio）与 ArkTS `AudioRenderer`（`on('writeData')`）是同一套音频框架的两条入口
  （同为 PCM S16LE 拉模型），等价。**默认**用 ArkTS 渲染器：渲染器与它的生命周期在 ArkTS
  （`workers/audio.worker.ets` + 门面 `services/AudioPlayer.ets`），跑在**音频独占 worker** 上
  （[`architecture.md`](architecture.md) §4.4）。
- 回调里用按**音源**分流的原生读口把环里的 PCM 拉进设备缓冲：RDP `audioRead(handle, buf)`、
  Moonlight `moonlightAudioRead`（后者无 handle，环是进程级单个）。
- 原生 OHAudio 是**可选的低延迟旁路**（`core/hmrdp_oh_audio.*`，Moonlight 用；见
  [`moonlight-native.md`](moonlight-native.md) §2.3）：写回调直接从同一个环拉 PCM，此时不建 ArkTS 渲染器。

## 3. 单一所有者：缓冲策略只在环里

- **生产者只解码 + 转发，不做缓冲/丢弃/节流**；RDP 的 rdpsnd 自带 overrun 守卫对本后端被关掉
  （patch step 35，见 [`rdp-native.md`](rdp-native.md) §3/§5）。缓冲/丢弃/预缓冲全在环一处。
- sink 的 `Write` 按**整包**入队、超 high-water 丢最老整包、返回当前排队时长（RDP 是 Wave Confirm 的真实
  渲染延迟）；渲染器的 `writeData` 回调用 `audioRead` 把环里的 PCM 拉进设备缓冲，环**始终填满**
  （数据 + 欠载静音），故回调总能报有效数据。
- 新的一段音频先**预缓冲**到目标深度才开始出声（启动静音不计损失），段内抖动**不**重新预缓冲；
  丢帧统计 = 欠载补静音 + 整包丢弃，仅在**数据仍在到达**的窗口内计入，静音停顿不计。
- 环的**延迟预算**（目标深度 + high-water，`SetLatencyBudget`）是**实例参数**，默认 120/300ms：RDP 用默认值；
  Moonlight 的低延迟路径设成由设备回调周期派生的小值（见 [`moonlight-native.md`](moonlight-native.md) §2.3）。

## 4. 格式变化

- 生产端检测到 rate/channels 变化时清空旧格式队列，并通知消费端（重新）建渲染器。RDP 走会话的
  `AudioFormat` 事件（`kAudioFormat`，见 [`rdp-native.md`](rdp-native.md) §5）；Moonlight 的格式在建流时
  确定（Opus 恒 48kHz、声道由协商定），不会中途重配，所以不需要该事件。
- 渲染器晚于首个格式事件启动时，用 `audioFormat(handle)` 读当前格式自行补建，不依赖事件一定被捕获。

## 5. 线程与锁

- sink 回调在库的解码线程（RDP 是 FreeRDP 播放线程、Moonlight 是 `AudioDecoderThread`），环的读取在音频
  worker（或原生 OHAudio 回调线程），两者只共享环的锁——渲染器回调不会被帧 / 剪贴板 / 文件的重活拖住
  （音频 worker 与后台 worker 分开的理由见 [`architecture.md`](architecture.md) §4.4）。`audioRead` 只在
  音频 worker 上调用。

## 6. 能力探测（音频重定向开关）

- 设备有无音频输出由 `DeviceCapabilities.audio()` 同步查询音频路由
  （`getDevicesSync(OUTPUT_DEVICES_FLAG)`）判定，设置页据此置灰「音频重定向」（Capability 模式见
  [`architecture.md`](architecture.md) §5）；原生只按 ArkTS 传来的 `enableAudio` 决定是否开音频特性
  （RDP `FreeRDP_AudioPlayback`；Moonlight 传空音频回调 ⇒ 静音），无输出设备时会话静音而不失败。
  `AppSettings.enableAudio` 是两协议共用的一个全局值。

## 7. 构建期 syscap 告警用注解屏蔽

- `@kit.AudioKit` 的 `audio` API 标注为"非全设备支持"，引用其值/枚举会打印
  `The system capacity of this api 'audio' is not supported on all devices`（WARN，非 ERROR）。该告警是工具误报
  （API 的 `@syscap` 标注带版本区间，与设备能力集比对不上；`audio` 在目标设备类型上实际可用），用
  `@SuppressWarnings({rules:[SuppressWarningsType.SYSCAP]})` 标注承载调用的**类**屏蔽（装饰器对类/方法生效，
  不能标自由函数；见 [`build.md`](build.md) §5）。运行时仍靠能力探测与优雅降级
  （无输出设备 ⇒ 关「音频重定向」、不建渲染器）兜底，不因该告警退回原生。
