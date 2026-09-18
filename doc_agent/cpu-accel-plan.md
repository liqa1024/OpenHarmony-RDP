# CPU（gdi）链路：并行与平台适配

> **定位**：这条线是"FreeRDP gdi 解码 + 我们自己的呈现器"的 CPU 路线的**并行部分**：执行器、宽度、
> 流水线分段、相位与内存归属。单核部分的**知识**（判据、账目口径、成本结构、量测纪律与陷阱、正确性
> 门禁、被否证的假设）在 [`gfx-engine.md`](gfx-engine.md) §8；协议框架见该文件 §0/§1；上屏见
> [`present-pipeline.md`](present-pipeline.md)；补丁与构建见 [`native-libraries.md`](native-libraries.md)；
> 历史口径与被否证的过程见 [`cpu-path_old.md`](cpu-path_old.md)（old）。

**CPU 路线** = FreeRDP gdi 解码 + 我们自己的呈现器。并行只发生在 **tile 解码这一段**（外加随之搬进去的
tile 合成拷贝，见 §1）。

## 0. 执行器与宽度（改这一段之前先读）

- **唯一执行器 = 平台任务队列（ffrt）**：按 FFRT 编程模型用**并发队列**（`ffrt_queue_concurrent`），任务带
  属性（name + 显式 QoS），屏障**按任务 handle 逐一等待**（不是"等整个进程的所有任务"）。宽度就是并发
  队列的 `max_concurrency`。
- **没有第二个执行器**：解码器经**弱符号**接平台队列（app 侧 `hmrdp_parallel.*`）；构建里没有这些导出时
  宽度读作 1 ⇒ 串行。FreeRDP 自带的 WinPR 池在这条链路上不参与，也不为它建池（`rfx.c` 在平台执行器
  可用时置 `UseThreads=FALSE`）。**不要再引入第二种池/队列方案或"切换开关"**。
- **宽度 = 设置里的「解码并行宽度」**（app 经 `HmrdpDecodeWidth()` 导出，值来自 `hmrdp_decode_tuning.*`）：
  `0` = 自动（全部在线核，上限 16）、`1` = 串行（接收线程直接解码，不建队列、不提交、不等待）、
  `2..8` = 手动。进程级，解码器在**每条 region 边界**按需读取。
- **入口符号**：`HmrdpParallelAvailable()` / `HmrdpParallelRun(tasks, fn, ctx)` / `HmrdpDecodeWidth()`、
  以及 dev 读数 `HmrdpParallelTakeMaxConcurrency()`。
- **不变约束**：**同一个 tile 仍由单个 callback 独占解码**（否则参考对比失去意义）。
- **两个变体开关**（给 A/B 用，默认都开）：`HMRDP_TILE_ARENA` = 每 tile 的三份持久缓冲用 surface 级连续
  arena；`HMRDP_WORKER_TILE_COPY` = tile 合成（拷贝）在 tile 解码段完成。

## 1. 流水线分段与账目口径

### 1.1 谁在哪条线程（实测形状）

| 段 | 线程 | 说明 |
|---|---|---|
| `zgx+parse` | 接收 | ZGX 解压 + PDU 解析 + 命令分类；读它前先看 `setup` 行 |
| `setup`（非像素命令） | 接收 | reset/create/fill/blit/**cache** 等；`cache` 在碎片样本可达每帧上百次 |
| `read` | 接收 | 一条 region 的位流解析（填 tile 元数据），随 tile 数增长 |
| **tile 解码 + worker 侧合成拷贝** | **ffrt（宽度个）** | **唯一并行段**；`dec` 是它的墙钟 |
| `update_tiles` | 接收 | 只留遍历与 O(1) 脏区 span 记账；像素拷贝已随解码段并行 |
| `compose` | 接收 | surface → primary |
| `present` | 接收 | 录制/上传/提交；GPU 侧另算 |
| `sync` / `presentWait` | 接收（阻塞） | 等 GPU 缓冲 / 等显示端，**不计 work** |

- **每两条相邻消息之间没有重叠**：`read`（解析 k+1）在 `dec`（解码 k）之后。跨消息流水线要动逐 tile 的
  预测器状态与 `region`/`params`/scratch 的复用，收益只有 `dispatch` 量级（见 §3）。
- `update_tiles` 的像素拷贝默认已在并行段（§1.2），所以并行下 `update` 只剩记账量级。

### 1.2 账目（`prog` / `prog2` / `run` / `energy`）

| 行 / 字段 | 含义 |
|---|---|
| `prog … read / dispatch / dec / update` | `read` 位流解析、`dispatch` 该消息任务提交（含建队列/句柄，串行）、**`dec` = tile 解码段**、`update` = `update_tiles` 整段 |
| `prog … calls / unions / tiles / tilesDec / ffrt` | `calls` 消息数、`unions`/`tiles` = 脏区并集与访问 tile 数、`tilesDec` 真正解码的 tile 数、**`ffrt` = 走平台队列的 region 次数** |
| `prog2 ms/frame (sampled 1/16)` | **`dec` 之内的拆相**。⚠ 并行下它的总量是**所有 worker 的时间之和**（≈ `dec` × 有效宽度）⇒ **只读占比**，不能与 `dec` 相加；采样探针必须是**线程本地**的 |
| `run threads= ffrt= cpu= cpuKHz=` | 该轮宽度 / region 实际派发次数 / **整轮进程 CPU 时间** / 频率档 |
| `energy: queue wall workerBusy ratio parMax` | 并行段墙钟 / worker 忙碌之和 / **有效宽度**（ratio）/ **实测最大并发**（parMax） |
| `energy: C1 / E2` | CPU 侧能量代理：`C1 = Σ busy×f`（cycle 代理）；`E2 = Σ busy×f²`（能量代理，β=2） |
| `workerPx / rdpPx`（dev） | 拷贝像素落在并行段与接收线程的比例 |

判读纪律：

- **宽度是否真的生效看 `parMax`**（应等于设定值）；**并行效率看 `ratio / parMax`**；`ffrt > 0` 才能证明
  解码确实在平台队列上。`ratio ≈ 1` 而宽度 > 1 ⇒ worker 没在做解码。
- **`dec` 有两种口径**（同一个"tile 解码段"，测点不同、不可混读）：并行时是**并行段的墙钟**（提交后到
  所有任务等完）；串行时是**本线程逐 tile 解码的时间**（此时 `dispatch` 为 0，不是缺失）。⇒ 读 `dec`
  前先看 `threads=`。
- **`calls` 与 `ffrt` 相等**表示每条 Progressive 消息只带一条 region（真实码流如此）⇒ "逐 region 屏障"
  实际就是"逐消息屏障"，**不要再假设一条消息里有多条 region**（§3）。
- **`cpu=` 的秒数不能当作"做了多少活"**：多核会把频率档压低。判据是 `E2`；`C1` 用来把频率因素剥掉。
- **量测纪律**：同会话、同录像、各档**交替**、N≥3 取中位数，跨会话/跨档的数字不可比；多轮走
  `native/scripts/replay-rounds.ps1`，等**这一轮**的 `state=finished` 再动下一步。

## 2. 已验证的结论（量级与相对关系）

- **并行对能耗是赚的**：整屏样本上，相对串行，帧墙钟快约 **1.5×**，进程 CPU 秒数约 **3×**，而 **`E2` 约为
  其一半**；宽度（2/4/8/14）之间 `E2` 差别落在重复噪声内。⇒ **"串行更省电"不成立**。
- **宽度是有效的**：`dec ≈ 解码段 CPU ÷ 宽度`，在宽度 2/4/8/14 上线性；`parMax` 与设定值相等。旧执行器
  （FreeRDP 自带的 WinPR 池）没有这个性质（请求 2 时实际并发远高于 2）⇒ 历史上"2/4/8 结果一样"的原因是
  **旋钮没生效**，不是"并行到顶"。这也是"宽度必须能从 `parMax` 读出来"的由来。
- **频率与工作量必须分开**：整轮 `C1`（整机 cycle）在串行与并行下几乎相同 ⇒ `cpu=` 的数倍差异主要来自
  **频率档**（串行能上最高频、并行被压在低档）。`C1` 是整机口径（本进程只占其中一部分），只能用来否掉
  "大幅额外工作量"，不能给解码自身定量。
- **相位归属比字节数更值钱**：并行下**访存型相位膨胀、算力型不膨胀**——`color`/`dequant`/`idwt` 每 tile
  相对串行高**数倍**，而纯位解码的 `rlgr` 只有**几成**。⇒ 并行段的额外开销在内存路径上，不在 bit decoder
  上。其中 `idwt`、`color` 跑的是**上游 NEON 内核**（`codec/neon/rfx_neon.c`、`primitives`），补丁改不到；
  能改的只剩 `rlgr`/`state`/`dequant`（单数字百分比量级）。
- **每 tile 持久缓冲的布局是次要但真实的项**：`sign`/`current`/`data` 从"每 tile 三次分配"改成 surface 级
  连续 arena（按 tile 连续、cache line 对齐）后，同宽度 A/B 下并行段墙钟与并行 worker 时间各降**个位数
  百分比**（`color` 最明显），连串行侧的 `update` 也一起降，能量在噪声内。
- **把拷贝从串行段挪进并行段是最大的一刀**：tile 解码完一块就按本消息的 clip 直写目标 surface（像素刚
  写完、cache 热，搬运随 worker 分摊），`update_tiles` 只留遍历与 O(1) 脏区记账 ⇒ 该段从 **ms 级降到亚
  ms 级**，整帧 `本机` 降约 **四成**，整屏样本 fps 提高约 **五成**；碎片样本的该段本来就小，因而持平。
  - 正确性约束：`update_tiles` 用的是"消息里最后一条 region"的合并 clip，而 tile 解码按 region 跑 ⇒
    两者用**折入消息序号的 clip 哈希**比对，一致才跳过拷贝，否则由 `update_tiles` 覆盖。门禁仍是两份
    录像的 `参考:对比` `bad=0`。
- **解码侧的正确性门禁**：参考画面（`bad=0`，两份基线录像各一套）+ 逆 DWT 对拍（`dwt check` 的
  `maxDelta`）——口径与量级见 [`gfx-engine.md`](gfx-engine.md) §8.4。

## 3. 已否证 / 容易走错的路

- **用裸 `ffrt_submit_f`（不限宽、无任务属性）换执行器**：比原池略差。那次既没有宽度控制也没有 QoS，
  **不能**当作"平台队列不行"的依据；正规用法是并发队列 + 任务属性 + 逐一等待。
- **"一条 Progressive 消息多条 region，所以要去掉逐 region 屏障"**：真实码流上每条消息只带一条 region
  （`ffrt == calls`），逐 region 屏障就是逐消息屏障，没有可合并的对象；为它做跨消息双缓冲流水线要动
  逐 tile 预测器状态与 `region`/`params`/scratch 的复用，而能省的 `dispatch` 只是 `dec` 的**几个百分点**
  ⇒ 不做。
- **把 `cpu=` 秒数直接读成能量或工作量**：多核低频下同一秒更便宜（§2）。
- **为碎片内容调低宽度来"省"**：碎片内容暴露的是并行效率问题，不是"不该并行"。宽度按 duty 选（§4），
  但目标是把并行做对，不是回避并行。
- **`update` 的掩码拷贝向量化**：实测中性（该循环受字节数限制），不再投入。
- **只按字节数找收益**：§2 的相位归属条目说明相位归属往往比字节数更值钱。
- **旧的 WinPR 池执行器**：请求宽度不生效（2/4/8 结果一样）；已整体移除，不要再作为兜底或对照组引入。

## 4. 优化候选（按实测排序）

| # | 项 | 要点 | 出口 |
|---|---|---|---|
| **P1** | **有效宽度** | 任务粒度（`HMRDP_FFRT_TASKS`）与领取块（`HMRDP_TILE_CLAIM`）按宽度对齐实测；目标把 `ratio` 拉向 `parMax` | 同一录像的 `ratio`/`dec`/`E2` |
| **P2** | **串行残量** | 碎片样本里 `zgx+parse`、`setup`（尤其 `cache`）、`present` 合计占比高；先量后改 | `perFrame` 分相 |
| **P3** | **`sync`（阻塞）** | 全屏 dirty 字节下的零拷贝单缓冲等待，属 present 侧；判据是 `sync` 与 `uploaded` 同步起落 | `sync` / `uploaded` |
| **P4** | **宽度按 duty 自适应** | 到达侧限速（duty 低）时回落 1，受限时用 2–4；判据用到达间隔/duty 加滞回 | 同一录像的 duty↔宽度曲线，`E2` 按档读 |
| — | **`dec` 本身** | 能改的相位只剩 `rlgr`/`state`/`dequant`；或提高有效宽度 | `prog2` 占比 + `dec` |

- **顺序**：P1 先（低风险、判据清晰），再按实测决定 P2/P3；P4 与 P1/P2 独立，可后置。
- **出口一律按 `E2`**（或同一 `cpuKHz=` 档下的 `cpu=`）读，不看单纯墙钟；`sync` 是等 GPU 的阻塞，
  **不计 CPU 能耗**。
- 任何一轮性能结论的前提是那一轮 `bad=0`；改了解码侧要附 `dwt check`（[`gfx-engine.md`](gfx-engine.md) §8.4）。
