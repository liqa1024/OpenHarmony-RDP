# CPU（gdi）链路：并行方案

> **定位**：只描述这条线**现在的并行形态**——并行范围、执行器、宽度、任务划分、内存归属、
> 合成归属——用于快速理解现状，不含结论与优化方向。单核知识、账目口径与判读纪律、正确性门禁
> 在 [`gfx-engine.md`](gfx-engine.md) §8；上屏见 [`present-pipeline.md`](present-pipeline.md)；
> 补丁与构建见 [`native-libraries.md`](native-libraries.md)。

实现位置：app 侧 `entry/src/main/cpp/hmrdp_parallel.*`、`hmrdp_decode_tuning.*`；
解码侧补丁 `native/scripts/patch-steps/` 11、20、21、22、22b、23、25（另 24 为探针门控）。
补丁对 app 模块的绑定方式是**弱符号**：没有这些导出的构建自动退化为串行。
**并行形态是固定的**：宽度、任务划分、执行器都不再是运行期开关（§5）。

## 0. 并行范围与线程归属

- **唯一并行段 = 一条 Progressive region 的 tile 解码**（`progressive_process_tiles`），外加
  随之搬进该段的 tile 合成拷贝（§4）。并行只发生在一条 region 之内。
- 其余各段都在**接收线程**（drdynvc 通道线程）：

| 段 | 线程 |
|---|---|
| ZGX 解压、PDU 解析、命令分类 | 接收 |
| 非像素命令（reset/create/fill/blit/cache…） | 接收 |
| region 位流解析（`read`，填 tile 元数据） | 接收 |
| **tile 解码 + tile 合成拷贝**（`dec`） | **接收线程 1 个 + ffrt（宽度 − 1 个）** |
| `update_tiles` 剩余部分（遍历 + 脏区记账，§4） | 接收 |
| surface → primary 合成、present 提交 | 接收（GPU 侧另算） |

- **消息间不重叠**：一条消息按 `read → dec → update` 接力完成后才处理下一条；没有跨消息流水线。
- **串行分支**（宽度 ≤ 1）：同一 tile 循环直接在接收线程执行，不建队列、不提交、不等待；
  工作缓冲用 0 号槽（§3）。

## 1. 执行器与宽度

- **唯一执行器 = 平台任务队列（ffrt 并发队列）**，由 app 的 `hmrdp_parallel.*` 提供，解码器经
  弱符号使用（补丁 11/21）：`HmrdpDecodeWidth()` 决定串行/并行分支；`HmrdpParallelAvailable()`
  / `HmrdpParallelRun(tasks, fn, ctx)` 提交与等待。符号不解析 ⇒ 宽度读作 1 ⇒ 串行。
- 队列形态（`EnsureQueue`）：`ffrt_queue_concurrent`，`max_concurrency` = 当前宽度 − 1；队列与每个
  任务都带属性（name + `ffrt_qos_user_initiated`）；屏障 = 逐任务 handle `ffrt_queue_wait`。
  宽度变化时销毁重建队列（仅在无任务在途时发生）。
- **调用方参与（caller participation）**：接收线程自己跑一个 chunk，队列只跑剩下 `宽度 − 1` 个
  （先提交、再跑自己的、最后等屏障）。region 用的线程数仍是宽度个，差别是其中一个从"停在屏障里"
  变成"干活"：没有线程在 tile 还可领取时空转；向 worker 池少要一个线程（自动档宽度 = 在线核数，
  池子可能给不出那么多）；且这一份跑在接收线程自己的上下文里（cache 热、QoS 是它自己的），
  而不是一个刚被唤醒的 worker 上。分块仍按 home + 段尾偷取，所以它做完自己的 home 会继续领块，
  快的那一侧自然多领。
- `HmrdpParallelRun` 的分支：`tasks <= 1` 或宽度 ≤ 1 在调用线程内联执行（此时参与就是全量内联）；
  `tasks > 128` 拒绝；单个提交失败的任务在调用线程补跑，保证一条 region all-or-nothing。
- 任务体内原子计数同时刻在途回调数 ⇒ `HmrdpParallelTakeMaxConcurrency()` 给出实测最大并发
  （dev 读数）。
- **WinPR 池不参与**（补丁 25）：平台执行器可用时 `rfx.c` 置 `UseThreads = FALSE`，不建池。
- **QoS**：tile worker 的 QoS 来自队列/任务属性；接收线程的 QoS 由 app 注册的钩子
  （`HmrdpSetThreadQoSApplier`，补丁 09）在 drdynvc 线程入口调用一次。
- **宽度 = 在线核数（上限 16）**，没有设置项：`hmrdp::DecodeThreads()` 只数 `sysconf` 的在线核。
  解码器在**每条 region 边界**按需读 `HmrdpDecodeWidth()`，所以进程一起来就是这个宽度，无需
  任何应用。性能核探测（`cpuinfo_max_freq`）只出现在日志行里，不参与宽度决策（§5）。

## 2. 任务划分（region → 任务 → tile）

- 每个 region 把 `region->numTiles` 个 tile 划成 `numChunks = 宽度`（cap 至 tile 数与
  `HMRDP_TILE_CHUNKS = 64`）个任务，一个任务一个 home 区间；任务描述符
  `PROGRESSIVE_TILE_CHUNK_PARAM` 携带共享参数表、scratch 槽与 home 数组（补丁 20/21）。
- **任务数按 region 的规模定**：`numChunks = min(宽度, numTiles / HMRDP_MIN_TILES_PER_WORKER)`，
  cap 至 `HMRDP_TILE_CHUNKS = 64`（`hmrdp_region_chunks()`，补丁 21）。一个线程至少要拿到
  `HMRDP_MIN_TILES_PER_WORKER` 个 tile 才值得被唤醒，所以**小 region 自动降档**：
  `numChunks <= 1`（即 `numTiles < 2 × 阈值`）时直接走串行分支，连队列都不提交。
  这是唯一的自适应点，也是唯一需要调的常数。
- **划分 = home + 段尾块偷取**（唯一形态，没有对照档）：任务 h 的 home 区间为
  `[h·N/K, (h+1)·N/K)`，K = `numChunks`。任务先解自己的 home——按 `HMRDP_TILE_CLAIM = 4` 个
  tile 的块用 `__sync_fetch_and_add` 领取；home 解完后按 `(homeIndex + round) % homeCount` 轮询
  偷取其他 home 的剩余块（同样按块领取）。尾部不均衡至多一块。**调用线程也是这些 home 之一的所有者**
  （§1），所以快的那一侧自然会多领。
- **不变约束**：一个 tile 仍由单次 `progressive_process_tiles_tile_work_callback` 独占解码；
  划分只决定 tile 由哪个任务、按什么顺序处理（同一条 region 的 tile 互不重叠，任意顺序像素
  等价）。

## 3. 内存归属

- **工作缓冲（scratch，补丁 20）**：per-context 一次性分配 `tileScratch` arena——64 个槽，
  每槽两块缓冲（系数工作缓冲 + 逆 DWT scratch，各 `(8192+32)×3` 字节）。chunk 回调把本任务的
  槽指针写进 `_Thread_local g_HmrdpTlsTileScratch`，tile 解码经 `hmrdp_tile_scratch()` 取用；
  串行分支固定槽 0。tile 解码路径不再走 `bufferPool` 的 Take/Return。
- **持久状态（补丁 22）**：每 tile 的三份跨消息常驻缓冲 `sign`/`current`/`data` 改为
  **surface 级连续 arena**（`HMRDP_TILE_ARENA = 1` 编译开关）：tile-major 布局——一个 tile 的
  三份在同一段连续内存、段起点 64 字节对齐；arena 随 tile cache 增长整体重建并复制旧块
  （预测器状态跨 resize 保留）；tile 结构不再拥有缓冲。首次绑定时显式清零
  （`sign`/`current` 为 0、`data` 为 0xFF）。
- **目标 surface**：tile 解码直接写入（§4）；多个 worker 写同一 surface 的不同行，互不重叠由
  划分保证（tile 互不重叠 + home 连续）。

## 4. worker 侧合成拷贝与 update_tiles

- **直写（补丁 23，`HMRDP_WORKER_TILE_COPY = 1`）**：`progressive_decompress` 把目标缓冲
  （指针/格式/步长/偏移/surface）暂存进 context；region 解码前按 region rects 建合并 clip
  （region16 union，与 `update_tiles` 同法）并计算 clip 哈希 `hmrdp_clip_hash`（折入每条消息
  递增的 `hmrdpMsgSeq`，跨消息不重复）。tile 回调**解码完成一块就**用该 clip 把 tile 直写
  目标 surface（`hmrdp_tile_copy_now`，几何守卫与 `hmrdp_composite_tile` 一致，
  `FREERDP_KEEP_DST_ALPHA`），并给 tile 记下 `hmrdpCopied` + 所用 clip 的哈希。
- **`update_tiles`（接收线程）**：工作集 = 「裁剪矩形覆盖到的 tile 范围 ∩ 本帧解码过的 tile」
  （补丁 22b：tile 记 `hmrdpFrameId`，`updateStamp` 保证每 pass 每 tile 访问一次）。对每个
  访问到的 tile：`hmrdpCopied` 且 clip 哈希等于本 pass 的 `hmrdpUpdateClip` 时**跳过像素拷贝**
  （只留脏区记账），否则照旧拷贝（后写覆盖）。脏区记账 = 每 tile 行一个 span
  （`hmrdpDirtyLeft/Right/Any`，O(1)；补丁 14）。region 解码结束清空暂存 clip。
- **结果不变性**：同一批 tile、同样的裁剪与像素、同样的脏区；门禁 = 参考画面 `bad=0` +
  解码侧对拍（[`gfx-engine.md`](gfx-engine.md) §8.4）。

## 5. 读数、探针与编译期开关

- **没有运行期开关**：宽度、任务划分、执行器都是固定的（§1/§2）。要对照，改代码重编——按
  [`gfx-engine.md`](gfx-engine.md) §8.1 的口径比 `dec` 墙钟与 `par` 的 `work`/`wall`，
  **不要**为了 A/B 在树里留第二套路径。
- **编译期开关**：`HMRDP_TILE_ARENA`（22）、`HMRDP_WORKER_TILE_COPY`（23）；常量
  `HMRDP_TILE_CLAIM = 4`、`HMRDP_TILE_CHUNKS = 64`、`HMRDP_MIN_TILES_PER_WORKER = 8`（§2 的自适应阈值）。
- **探针**：`HmrdpProgStat[24]`（`read`/`dispatch`/`dec`/等待、tile 计数、ffrt region 计数、
  worker/串行侧拷贝像素、1/16 采样的 per-phase 拆相），由 `HmrdpSetProgSample` 门控（补丁 24）；
  `parMax = HmrdpParallelTakeMaxConcurrency()`；`energy` 行（`hmrdp_energy.*`）。
  账目字段与判读纪律见 [`gfx-engine.md`](gfx-engine.md) §8。
- **并行段账目「par」**（app 侧 `hmrdp_parallel.*`，与 `HmrdpSetProgSample` 同开同关）：在**任务边界**
  计时，用**本 region 的 chunk 数**（`tasks`，也就是这个 region 实际选用的线程数：调用线程自己那个
  加提交出去的那些）算容量：

  | 值 | 定义 | 量纲 |
  |---|---|---|
  | `wall` | 接收线程从开始提交到全部任务等完 | 真实时间 |
  | `capacity` | `tasks × wall`：该 region 主动要的线程时间 | 折叠量 |
  | `work` | `Σ` 回调执行时长（即 tile 解码段，含 worker 侧合成拷贝） | 折叠量 |
  | `idle` | `capacity − work`：这份线程时间里没跑回调的部分（导出量） | 折叠量 |
  | `wait` | `Σ(任务开始 − 提交)`：任务排队延迟，**单列** | 折叠量 |
  | `Kavg` | `capacity / wall`：按墙钟加权的平均实际线程数（导出量） | 折叠量 |

  - **为什么按 `tasks` 不按宽度**：任一时刻在跑的回调 ≤ `tasks`（提交出去的那些加调用线程自己那个）⇒
    `work ≤ capacity` 恒成立、`idle` 恒非负。而 chunk 数本身就是**这条线唯一自适应的量**
    （§2：region 越小开得越少），账目必须跟着它走，否则 `busy`/`idle` 会把"阈值故意没用满的宽度"
    算成闲置。逐任务的「提交前空闲 / 完成后空闲」这类把测量绑死在某一种划分上的口径仍然不用。
    ⚠ `tasks × wall` 是"**要了**多少线程时间"，不是"池子真给了多少"：池子给不出（自动宽度 = 在线
    核数）时，要的那部分仍会被算进 `capacity`，体现为 `idle` 与 `wait` 一起变大。
  - **`wait` 不并进容量**：任务排队时 worker 正在跑别的任务，二者是同一段时间的两面 ⇒
    `work + wait` 可以超过 `capacity`。`work`/`idle` 才是线程账，`wait` 只作队列延迟读。
  - **自检**：① `work ≤ capacity`（`idle` 为负 ⇒ 回调被重复计时，例如提交失败回退到接收线程内联
    执行）；② `work ≈ HmrdpProgStat[9]`（同一批回调的另一路折叠和，两路独立互证）。
