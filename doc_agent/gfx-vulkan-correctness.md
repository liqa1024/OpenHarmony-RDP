# Vulkan 引擎正确性（引擎 vs gdi 逐像素对拍）

本文件描述**引擎与 FreeRDP gdi 的逐像素一致性**这条线：验收口径、两份录像的差异、**已经修掉的分叉**
（含依据与要盯住的量）、**仍未修完的残留**（含精确坐标与下一步）。协议/合成语义仍以
[`gfx-engine.md`](gfx-engine.md) §0~§2 为准；kernel 性能与并行化见
[`gfx-progressive-kernel.md`](gfx-progressive-kernel.md)。

---

## 1. 背景与口径

### 1.1 验收门

dev 回放页三条路线：`CPU`（纯 gdi，性能参照）、`Vulkan`（只跑引擎）、
**`Vulkan对比`**（同一条流同时喂 gdi 与引擎，每 30 帧采一次整屏比对）。验收门是
**`compare(GPU vs gdi): checks=… bad=0 rgbPx=0 maxDelta=0`**——先测正确性，再看性能。

对拍两侧：

- 引擎侧：`GfxVkDesktop::ReadScreen()`（引擎的屏幕镜像 image，**不是**呈现器/swapchain，
  所以换呈现后端不影响 `bad`）；
- 参考侧：gdi 自己的 `gdi->primary_buffer`；
- 需要逐命令定位时：`GfxVkDesktop::ReadSurface/ReadSurfaceRect` 与 gdi 的 `gdiGfxSurface::data`
  （`GfxCpuDesktop::SurfaceData`）比较——**只以 gdi 自己的表面为准**。

### 1.2 两份录像（本地 `.cache/`，不入库）

| 文件 | 场景 | 现状 |
|---|---|---|
| `hmrdp_gfx.bin` | 浏览/滚动（消息稀疏、脏区小） | `checks=21 bad=0 rgbPx=0`，`frames=659`；**性能基线与正确性基线都用它** |
| `hmrdp_gfx_video.bin` | 看视频（整屏大块变化、每帧 4~7 条 Progressive 消息） | 尚有残留（§3），**从未通过过 bad=0**，不要拿它当性能基线 |

两次录制**同名不代表同一份流**：换样本前把设备里的 `hmrdp_gfx.bin` recv 回 `.cache/` 留档。
设备侧路径固定为 `/data/app/el2/100/base/com.lixa.hmrdp/haps/entry/files/hmrdp_gfx.bin`，
宿主只能**覆盖已存在的文件**（`hdc file send` 到该目录，目录内不能新建文件）。

### 1.3 定位分叉的顺序（按代价从小到大）

1. **整屏对比**（`Vulkan对比`）：给出 `bad/rgbPx/maxDelta/bbox`、逐 64 行/逐 tile 的差异分布
   （`compare tiles: grid=… touched=… full=… half=…`），首次分歧还会写 `<capture>.cmpdump`；
2. **逐命令 A/B**（`kCodecAbEnabled`，默认关）：每条命令后拿"该命令声明要写的 rect"与 gdi 自己的表面
   比，第一个失败的命令即元凶（label 会带消息序号与形状，如 `progressive#28(161/0/76)`、`restamp`、
   `clearcodec`、`upload`）。它**天然漏掉 Progressive 的"重复合成"（restamp）**——那些 tile 不在
   "本消息声明要写"的 rect 里，所以实现里另镜像了一份 FreeRDP 的 frame-tile 列表（按 frame id 变化
   清零），把每条消息会重复合成的 tile 一并纳入检查（label `restamp`）；
3. **整屏 A/B**（`kSurfaceAbEnabled`，默认关）：每条命令后比整个表面，用来看"分叉是在哪条命令之后
   才出现的"（逐命令 A/B 只能看到它自己 claim 的 rect）；
4. **状态级对拍**（`kTileStateAbEnabled`，默认关）：逐 (tile, 分量) 比 **解码状态**——
   引擎 `surface->rfx.{cur,sign,bp}` vs FreeRDP `tile->current/sign/yBitPos`（见 §3.3）。

---

## 2. 已修的分叉（都有实测依据）

### 2.1 整块 tile 的全屏 stale 带（成因：compose dispatch 超出每轴工作组上限）

- **症状（修前）**：`bad=6`、`rgbPx≈10.3M`、`maxDelta=255`、bbox 覆盖整屏；逐 64 行统计显示
  **下半屏（tile 行 24~32）100% 像素全错**，上半屏只有数值级差异 ⇒ 呈现器画面下半屏停在上一帧。
- **成因**：`rfx_compose` 是"一个 invocation 一个像素"，主机按**线性下标**派发
  `(tileCount*4096+63)/64 = tileCount*64` 个工作组；整屏消息有 1000+ 个 tile ⇒ 需要 6 万以上
  （实测最大 **91008**），超过设备 `maxComputeWorkGroupCount[0]=65535`：平台**静默只跑一部分**，
  消息尾部的 tile 从未被合成。浏览录像也有 1 次 103488 的超限，只是内容近似静止、随后被重合成掩盖。
- **修法**：派发改 **2-D 网格**（主机按 65535 拆 `gx*gy`），着色器用
  `gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x` 还原线性 index。
  `Stats()` 的 `composeGridSplits`/`composeGroupsMax` 只要不为 0 就说明这条路正在生效
  （视频 `188 / 91008`，浏览 `1 / 103488`）。
- **强约束**：**任何"每像素一个 invocation"的 kernel 都必须照此处理**；`rfx_idwt`（一个工作组一条
  stream）与 `rfx_decode`（`(streams+63)/64`）在 `kMaxBatchStreams=6144` 下都远低于上限，但也别让
  新 kernel 直接用线性 `tileCount*64`。
- **修后**：`rgbPx=2.82M`、`maxRgbPx=0.50M`，全屏 stale 带消失（残余见 §3）。

### 2.2 UPGRADE 细化被参考实现拒收时，引擎仍把像素写出去

- **协议语义**（FreeRDP `progressive_rfx_upgrade_component` 末尾）：`aSrlLen/aRawLen` 与声明的
  `srlLen/rawLen` 不符 ⇒ **整块 tile 被拒收**——`tile->data`（像素）不写，但 `current/sign/位置状态`
  已经吃掉这次细化；而且该 tile 仍留在本帧的 tile 列表里，之后每条消息都会**按旧像素**重新合成它。
- **引擎原来没有这个校验**，会把 gdi 丢掉的细化照常合成 ⇒ 稀疏、整块、小幅色偏。
- **现在**：`rfx_decode.comp` 用 **FreeRDP 的记账口径**（`BitReader.cons` = *请求过*的位数：越界也照加，
  `BitStream_Shift` 对 ≥32 的位移不计数）算填充后的字节数并做校验，判定写进每条流的位状态条目
  **闲置字节 10/11**（band 0~9 占 0~9）；`rfx_compose.comp` 用**绑定 4** 读该条目，任一分量被拒 ⇒
  **整块不合成**（kFirst 解码成功时清标志、type 3 重合成不清，于是"像素是旧的"一直保持到该 tile
  重新解码成功）。`Stats()` 的 `rejectedTiles` 同源可见。
- **依据**：`kLogUpgradeLengths` 探针（`mismatched/streams per msg`，进 `Stats()`）实测视频 `#12` 走
  6/6 全中，而**浏览录像 3000+ 条 upgrade 流全部通过**——后者同时说明"读取器/长度语义没问题"。
- 同一处还有 **`quality` 边界**：`quality >= numProgQuant`（且 ≠0xFF）时 FreeRDP 也整块拒收；
  `quality == 0xFF` 用的是**全零**的 `quantProgValFull`。前者已按参考实现跳过
  （`rejectedTiles`，两份录像都为 0），后者本来就是全零。

### 2.3 参考实现侧：`progressive_tile_new` 的 `current`/`sign` 未初始化

- `tile->data` 被填 0xFF，但 `tile->current`/`tile->sign` 直接来自 `malloc` 且**从未清零**；而
  DIFFERENCE / UPGRADE 是**先读后写**的预测器状态。编码器假定客户端状态初值为 0，所以参考解码器的
  输出取决于堆里恰好有什么（对拍不可复现）。
- 已作为**第 8 条补丁**修掉（[`native/scripts/patch-freerdp.ps1`](../native/scripts/patch-freerdp.ps1)，
  源码同步改在 `native/third_party/FreeRDP/libfreerdp/codec/progressive.c`），重编 `libfreerdp3.so`
  并放回 `entry/libs/arm64-v8a/`。**副作用：本录像的对比数字逐位不变**（残留不是这条，但参考侧的
  确定性/正确性必须修）。

### 2.4 两处"照抄参考实现"的语义修正（本录像测不出差别）

- **UPGRADE 的 `numBits` BYTE 回绕**：`RFX_COMPONENT_CODEC_QUANT` 字段是 `BYTE`，
  `numBits = tile->yBitPos - yBitPos` 在"本消息比该 tile 上次更粗"（`ob < nb`）时回绕成 ~226~255；
  此时 FreeRDP 的 `BitStream_Shift` 对 ≥32 的位移**什么都不做**，而读出的值仍按硬件 5 位掩码取向 ——
  结果是**读到垃圾且不消耗位**，长度校验照样通过。引擎原来把它钳到 0（等于跳过该带）。现在按参考
  实现照抄（`brReadBits` 的 `n>=32` 分支 + `srlRead` 的 `maxMag`，并把一元循环上限收到 32768 以免
  零填充尾上自旋 40 亿次）。
- **WBT 状态机逐消息清零**：FreeRDP 在 `progressive_decompress` 开头 `progressive->state = 0`，
  "REGION 在 FRAME_BEGIN 之前 / FRAME_END 之后"这条忽略规则只看**本条消息**的块；引擎原先把
  `frameBegin/frameEnd` 跨消息持久化，可能接受 gdi 跳过的 region（本录像 `skippedRegions=0`）。

---

## 3. 残留（仍未收窄到具体一行）

### 3.1 症状与分布

- 整轮 `checks=6 bad=6`，`rgbPx≈2.82M`（每帧 0.45~0.50M）、`smallDeltaPx≈1.33M`（其中 ≤2 的占 ~47%）、
  `maxDelta=255`、bbox 仍是整屏；逐 tile 摘要 `touched=246 full(≥4000px)=48 half(≥2048px)=67`。
  逐行看最大贡献在屏幕最下 3 个 tile 行（任务栏一带的高频内容）与少数离散 tile。
- 逐命令 A/B 反复指到同一类指纹：`rect=(2560,192)+64x64 bad=3979 maxDelta=4`，
  `engine=b16 g17 r13 / gdi=b18 g17 r15`（**G 相同、B/R 同降 2**）——整块均匀的**色度**偏移，
  正是 DC（LL3）带出错的形状。

### 3.2 已排除（下列每一条都**没有**改变 `rgbPx`，故都不是元凶）

- `nonExtrapolate=0`、`multiRegion=0`、`frameIdRepeats=0`、`skippedRegions=0`（双方 tile 集合一致）；
- 3 条畸形消息全是 `region-rects-range`（region 头就被 FreeRDP 的 `numRects` 校验挡下 ⇒ 双方都不做事；
  `Stats()` 的 `batchOverflow` 也必须为 0）；
- **restamp（type 3）**：把重合成整体关掉（`kRestampEnabled=false`）分叉量几乎不变
  （2.7655M vs 2.7650M）；补上 restamp 的逐 rect A/B 后，命中的仍是 **DIFF 为主的 kFirst 消息**；
- §2.2 / §2.3 / §2.4 的全部改动（已修但不改变数字）；
- 未压缩位图：捕获里只有 3 条、都是 `format=0x20040888`（**BGRX32**，与引擎"按 BGRA 字节序直接拷贝"
  的假设一致）且只有 4 行高——那 11ms 是 `SyncCpuAccess` 的 fence，不是拷贝；
- 路由顺序（对比路线是否为进程内第一个 gdi 上下文）不影响数字 ⇒ 不是堆残留、可**逐位复现**。

### 3.3 状态级对拍的结论（工具已就绪，结论待复核）

工具（§4 的 `kTileStateAbEnabled`）：FreeRDP 侧导出访问器 `HmrdpProgressiveTileState` 读
`tile->current/sign/yBitPos`；引擎侧 `GfxVkDesktop::ReadTileState`。逐 (tile, 分量) 比
`cur`（去量化后的系数）/`sign`（RLGR 原值）/`bitPos`（每带 1 字节，**带序** HL1…LL3）。

- **坑**：gdi 的 Progressive 状态挂在 **`surface->codecs->progressive`**
  （`gdi_SurfaceCommand_Progressive` 用的就是这个），**不是** `context->codecs`——读错那个会一律
  `unavailable`。
- `#1..#12` 的全部 tile（80 个）状态**逐系数一致**（含此前被怀疑的 tile (47,2)/#9）；
- 首个状态分叉：`msg=#14 tile=(40,3)`（**单 tile、98 字节**的 kFirst），命中 **Y 分量、下标 4015**，
  把命中下标前后各 5 个值打出来后：

  ```
  curE(4010..4020) = 0 0 0 0 0 -3584 -3520 -3520 -3520 -3520 -3456
  curG             = 0 0 0 0 0 -3552 -3520 -3520 -3488 -3488 -3456
  signE = signG    = 0 0 0 0 0 0 0 0 0 0 0        band=9  bpE=bpG=7
  ```

  两侧都是"直流台阶"（LL3 = DC 带），但**台阶高度差一倍**（引擎每级 +64、gdi 每级 +32）。

### 3.4 下一步（先怀疑工具，再怀疑引擎）

这组数字**自相矛盾**：`bitPos=7` 按两侧代码都推出移位 = 6（引擎 `sh[i]=nb-1`、FreeRDP
`quant+progQuant-1`），那 gdi 的台阶就不该是 ×32（移位 5）。所以顺序是：

1. **先自检探针**：让访问器再返回 `tile->yQuant.LL3` / `tile->yProgQuant.LL3`，检查
   `bitPos[9] == yQuant.LL3 + yProgQuant.LL3`。不成立 ⇒ **访问器的"带序"映射错了**（本轮的数字随之
   作废），先修探针再谈引擎；
2. 成立再并排看**引擎侧 `qa[9]`/`pa[9]`/`sh[9]`**（`DecodeProgressive` 在**主机**上算的，直接打印最省事）
   与上面三个 gdi 值：
   - 输入 nibble 不同 ⇒ **量化表选择**不一致（`quality` / `quantIdx` / `quantProgValFull` 的对应）；
   - 三者全同而移位仍差 1 ⇒ 落在 `progressive_rfx_quant_lsub` 的 BYTE 语义 / 引擎 `sh[i]=nb-1` 上；
   - 若确认是"该带去量化移位"差 1：记住**去量化前的值就是 `sign`**（差分 `rfx_differential_decode`
     在去量化**之前**跑），且**抽取（extrapolate）与非抽取两套带表不能混用**：抽取路径
     `0/1023/2046/3007/3279/3551/3807/3879/3951/4015`（长度 1023/1023/961/272/272/256/72/72/64/81，
     差分在 4015/81），非抽取路径是 `0/1024/…/4032`（长度 1024/1024/1024/256/…/64，差分在 4032/64）。
     kFirst（`progressive_rfx_decode_component`）与 UPGRADE（`progressive_rfx_upgrade_component`）
     用的是**同一套**抽取表（本录像 `nonExtrapolate=0`），要核对的是**去量化前是否有一步漏做/多做**。
3. 复现很便宜：`kTileStateAbEnabled=true` + `kTileStateAbMessages=16` 就能命中 `msg #14`
   （单 tile、98 字节），不必跑完整轮。

---

## 4. 开发开关一览（都在源码常量里，默认关）

| 位置 | 常量 | 作用 |
|---|---|---|
| `hmrdp_replay.cpp` | `kCodecAbEnabled` | 逐命令 A/B（progressive 自身 tile / clearcodec / upload / fill / cacheRestore / restamp） |
| 同上 | `kSurfaceAbEnabled` | 每条命令后比整屏表面（看"哪条命令之后分叉出现"） |
| 同上 | `kTileStateAbEnabled` + `kTileStateAbMessages` | 状态级对拍（§3.3），每消息一次 submit |
| 同上 | `kDumpCompareScreens` | 首次分歧写两侧整屏 PPM（~19MB/张，落到 filesDir） |
| `hmrdp_vk_desktop.cpp` | `kLogUpgradeLengths` | upgrade 长度校验的 `mismatched/streams per msg` 探针 + 未压缩命令的 `format` 日志 |
| 同上 | `kLogProgressiveMessages` | 前 40 条 Progressive 消息的形状（tiles/first/upg/diff/rects/clip/bytes） |
| 同上 | `kRestampEnabled` | 关掉"重复合成"做二分（**诊断用**，默认开） |

诊断工具只在定位时开，**查完立刻关**：它们都会插入 submit/fence（状态/整屏 A/B 每消息一次，
逐命令 A/B 每命令一次），会拖慢回放并可能撞上 120s 的安全上限。

---

## 5. 纪律

- 改引擎 / 着色器 / FreeRDP 补丁后**先跑两份录像**：`.cache/hmrdp_gfx.bin` 必须仍是
  `bad=0 rgbPx=0`（回归门），再看 `.cache/hmrdp_gfx_video.bin` 的 `rgbPx` 有没有下降。
- **不要用"跳过某条 dispatch"做归因**：会改变后续数据相关负载且会花屏；用 §1.3 的分级 A/B。
- 改了 FreeRDP 源码/补丁要**重编 `libfreerdp3.so` 并放回 `entry/libs/<abi>/`**（见
  [`native-libraries.md`](native-libraries.md)）；`entry/libs/` 与 `native/third_party/` 都不入库。
- 拼日志前先 `hilog -r`；周期性统计行会很快冲掉缓冲区，关键量尽量进 `Stats()`（回放页会显示）
  或尽早抓取。
