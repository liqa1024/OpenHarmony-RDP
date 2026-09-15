# Progressive kernel 优化（未完成）与新录像正确性残留

本文件是**后续工作清单**，不是口径文档：方向、依据、已踩过的坑与验收方式。
改 GFX 语义前仍以 [`gfx-engine.md`](gfx-engine.md)（尤其 §2.2 与 §3）为准；量测口径也在那边。

---

## 1. 现状

- `Vulkan对比`（引擎 vs 离线 gdi 逐像素比对）在 **`.cache/hmrdp_gfx.bin`（浏览/滚动）** 上
  `checks=21 bad=0 rgbPx=0`、整份跑完（实测 `frames=659`）——这份捕获是**当前唯一可信的性能基线**。
- 已落地的三项（都已过 `bad=0`）：host-visible 改 `HOST_CACHED` + 范围级 flush/invalidate；
  `rfx_compose` 改按像素并行；逆 DWT 拆成独立 kernel（`rfx_idwt.comp`，系数走 shared）。
- **未完成**：RLGR 解码 kernel（`rfx_decode.comp`）的并行化；视频录像的正确性分叉。

---

## 2. RLGR 解码 kernel 的并行化（producer / consumer）

### 2.1 为什么值得做（依据）

- RLGR 是**逐符号自适应的变长位流**（`k`/`kr` 由前一条符号的游程驱动，位指针前进量依赖数据），
  **一条 stream 内部的位解码无法并行**：重启点（编码器不插同步标记，我们改不了）、
  投机解码+校验（段边界未知）、指针跳跃（符号长度与自适应状态无关这一前提不成立）都不成立。
- 但**一条 stream 的工作量大部分不在位解码**：实测每条 kFirst stream 只有 **48.7 个 RLGR 符号**
  （用 counter SSBO 直接统计），却要落 **4096 个系数**（`coef` + `cur` + `sign` 三份）。
  也就是说 ~1.2% 的"交接"覆盖了 100% 的系数输出。
- 因此可行设计是 **lane 0 只跑串行符号状态机，把 (起始, 长度, 值) 发布到 shared 队列，
  其余 63 个 lane 并行写系数**（"串行状态机 + 并行量产"），dispatch 从 `ceil(streams/64)`
  变成 `streams` 个 workgroup（与表现良好的 `rfx_idwt` 同构）。
- 预期上限（不吹）：若输出侧占 x，加速 ≈ 1/((1−x) + x/64)；按上述比例，kFirst 有望 3~5x。
  **upgrade（type 2）不适用**：实测每条 upgrade stream ~3243 次"逐系数"迭代（每系数都要单独读变长位），
  只能交接"状态写入"部分；视频场景里它只占 6%。

### 2.2 实现要点（缺一条就会错）

- **barrier 纪律**：`sid >= uNumStreams` 这类早退必须**按 workgroup 一致**（用 `gl_WorkGroupID.x`
  判定时天然一致）；**绝不能按 lane 早退后再 `barrier()`**（UB，实测花屏 + present 失败）。
- **补零尾的终止条件不能用 `brRemaining()`**：它是"窗口内还有多少位"，位流耗尽时可能仍 >0。
  必须由解码函数显式置一个 `gExhausted` 标志，循环头据此补零尾并置 done——否则**死循环**
  （表现为 GPU 空转、画面大片中性灰、dispatch 迟迟不完成）。
- **系数平面的整字写要区分"值"与"零"**：值落在**奇下标**时，该 32bit 字的低半字属于
  **上一条目的值**（不是本游程的零），只有 `count > 0` 时才可整字覆盖，否则只能改高半字。
  `count == 0`（无零游程、纯值）在 RLGR 里非常常见。
- **队列容量**：按实测 48.7 条/stream，256 条足够；满时分批"发布→排空"，批次之间的状态
  （`k/kp/kr/krp`、位指针、`widx`）必须跨批保留。
- **先在 host 侧对拍**：把 drain 的"词/半字/边界"运算写一个纯 C++ 副本，用合成 entry 列表与参考
  writer（`wwPut`/`wwFlush` 语义）逐字节对拍 —— §2.2 的整字覆盖 bug 本来能被这一步当场抓住。

### 2.3 已知未解问题（上次实现的终点）

修复下述两个 bug 后，producer 版**仍然既慢又错**，与会话终点一致：

- 症状：`frames=49`（基线 659）、`fps=0.4`、`present>400ms/帧`、
  `gpuMs … (samples 3/3/3)`（44 个批里只有 3 次 decode dispatch 完成）、`compare bad=1`（整屏）。
- 批记账本身是对的（`batches flushed=44 skipped=0 maxStreams=4851`，可放心复用）。
- **未定位**：怀疑在 shared 队列的发布/排空交接、或 drain 的边界字运算、或 barrier 造成停摆。
  下次请**先加最小诊断**（每批的 entry 数、排空后的校验和、批次数），再谈性能。
- 另有一个**强约束**：`rfx_decode.comp` 是**寄存器极度敏感**的 kernel —— 实测只加两个计数器 + 一个
  binding 就慢 2x。因此建议**按 stream 类型拆成三条独立 kernel**（type 0/2/3），每条只保留一条代码路径，
  而不是在一个 kernel 里堆三套逻辑。

### 2.4 已修的 bug（保留供参考，勿重犯）

1. 生产者循环里"位流耗尽返回 false 后 `continue`" → 死循环（见 §2.2 的 `gExhausted`）。
2. 奇下标整字覆盖吃掉前一个系数（见 §2.2 的 `count > 0`）。

### 2.5 已否决的相邻方案（不要再试）

- **跨消息合并 decode**：与 restamp（type 3）的 `cur` 时序互斥，见 [`gfx-engine.md`](gfx-engine.md) §2.2。
  另外实测"消息聚簇度"也不够：浏览场景多为 0~1 条/帧。
- **把 payload 整段搬进 shared**：只快 17%，且 16KB shared 把常驻 workgroup 压到每 SM 2 个、
  整系统慢 3 倍。
- **32bit 字缓存**（减少 payload load 次数）：无变化。
- **用"跳过某条 dispatch"做归因**：偏差可达数倍且会花屏；改用 timestamp query（见 §3 与
  [`gfx-engine.md`](gfx-engine.md) §3/§6）。

---

## 3. 视频录像（`.cache/hmrdp_gfx_video.bin`）的正确性残留

- 症状：`Vulkan对比` 为 `bad=6`（**整屏**、`maxDelta=255`、`rgbPx≈10.3M`），`checks=6`。
- 已排除：与批量/解码的改动无关 —— 在**已验证正确**的代码路径上（关闭消息合并、per-lane 解码）
  同样是 `bad=6`。**这份录像从未通过过对比验证**，属既有分叉。
- 该场景的重负载特征（可作为排查线索）：
  `diff`（`RFX_TILE_DIFFERENCE`）与 restamp 占比远高于浏览场景；每帧 4~7 条 Progressive 消息；
  捕获里含 **3 条畸形消息**（`rfxParse errors=3`，FreeRDP 也直接拒收 ⇒ 双方行为一致，不应是元凶）。
- 建议路径（按顺序）：
  1. 用 harness 的**首次分歧材料**：`<capture>.cmpdump`（逐像素 e/g 值，首次分歧时自动写）+ 它给出的
     bbox，先判断是"未解码区域（全 0 系数 ⇒ 中性灰）"还是"数值算错"；
  2. 需要"是哪条命令分叉"时，开 `kCodecAbEnabled` 的**逐命令 A/B**（判定只以 **gdi 自己的表面**为准；
     开启后极慢，查完立刻关）；
  3. 重点怀疑 DIFFERENCE（`flags & 1`）与 restamp（type 3）在**大 region / 同帧多条消息**下的组合路径。
- 注意：排查期间**不要把这份录像当性能基线**；性能结论仍用 `.cache/hmrdp_gfx.bin`。
- 采集文件是**设备沙箱内**的 `hmrdp_gfx.bin`，宿主只能**覆盖已存在的文件**（`hdc file send` 到
  `/data/app/el2/100/base/com.lixa.hmrdp/haps/entry/files/`，目录不可创建新文件）。
  **换样本前先把它 recv 回 `.cache/` 留档**：一旦被覆盖，同名的新录制就取代了旧录制，
  而"同名"不代表"同一份流"（帧数、消息分布都可能不同，基线也随之失效）。

---

## 4. 会话遗留的代码清理项

以下都是"为了做实验"而留下的脚手架，功能上不影响正确性，但应当清掉（需能跑 `bad=0` 复验）：

| 位置 | 内容 | 说明 |
|---|---|---|
| `hmrdp_vk_desktop.cpp` | `kBatchMergeMessages = false` 开关 + `AppendToDecodeBatch`/`FinishDecodeBatch`/批状态 | 跨消息合并已被否决；现在等价于"一条消息一个批"，可以退回到原来的逐 chunk 循环（少一层间接） |
| 同上 | `batchesFlushed/batchesSkipped/batchStreamsMax/messagesThisFrame/messagesPerFrame[]`、`chunkBytes*`/`streamCount` 及其 `Stats()` 输出 | 诊断计数；`gpuMs`/`perChunkMs`/`decode input` 这类**通用**指标建议保留 |
| `rfx_decode.comp` | 已被 `rfx_idwt.comp` 取代的 DWT 参考实现、`dequantSub`、`WordWriter` 等 | 确认无人调用后删除（删前跑一次 `bad=0`） |

---

## 5. 复用的量测口径（细节见 gfx-engine.md §3/§6）

- 每条 dispatch 的 GPU 时间：引擎 `Stats()` 的 `gpuMs rlgr=… idwt=… compose=…`（timestamp query，
  两端 `COMPUTE_SHADER`）。
- 内存类型与带宽：`ProbeHostMemory`；空提交固定开销：`ProbeSubmitCost`（都是进程内一次）。
- 整轮口径：回放页每轮都 `stopReplayTest()+start`，**必须等 `(running=0)` 再取数**；用轮询而不是固定 sleep；
  `uitest dumpLayout` 的输出**不是合法 JSON**，用正则抓 `route=…(running=N)`。
- 验收门：`Vulkan对比` 的 `bad=0 rgbPx=0`（先测正确性，再看性能）。
