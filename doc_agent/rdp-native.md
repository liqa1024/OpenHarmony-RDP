# RDP 原生（FreeRDP 补丁 / 音频 sink 侧）

FreeRDP / zlib / OpenSSL 的**源码构建与补丁体系**（产物不入库、单一构建入口、依赖布局、补丁系统规则）
是两协议共享的，见 [`native-build.md`](native-build.md)。本文件只保留 **RDP / FreeRDP 侧**的补丁内容、
编译选项与音频的 rdpsnd sink 侧；两协议共享的音频播放链路见 [`audio.md`](audio.md)。

## 1. 产物不入库

见 [`native-build.md`](native-build.md) §1。

## 2. 构建流程

见 [`native-build.md`](native-build.md) §2 / §3。

## 3. 补丁脚本在做什么（以及为什么必须保留）

**平台兼容**

- musl/OHOS 下 `pthread_cancel` 等不存在，需按平台屏蔽/替换。
- OpenSLES 标识符映射：OHOS 只有标准 OpenSLES 1.0.1 头，没有 Android 扩展。

**链接与符号**

- client/common 编成**共享库**（默认静态会导出不出 NAPI 侧需要的符号）。
- **无版本号 SONAME（所有库同一个口径）**：FreeRDP 配合 `-DWITH_LIBRARY_VERSIONING=OFF`；zlib 由
  `patch-zlib.ps1` 去掉 `SOVERSION` 与 `VERSION`（CMake 从 `SOVERSION` 取 SONAME、缺省时退回 `VERSION`，
  所以两个都得去）；OpenSSL 由 `native/openssl-cmake` 直接产出不带版本的 `libcrypto.so` / `libssl.so`。
  **判据是 SONAME == 打包文件名**：`native/install/<arch>/*/lib` 里多出一个 `libX.so.1` 就说明版本号没去掉。
  注意 version script 带来的**符号版本**（例如 zlib 的 `ZLIB_1.2.0`）与文件命名无关，不需要处理。
- 上游在该库上开了 `-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON`，所以**导出符号打错/漏定义只有到设备 dlopen
  时才炸**（症状：`relocating failed: symbol not found` + `does not provide an export name …` + 启动即退）——
  改完补丁用 `llvm-nm -D --undefined-only libwinpr3.so | findstr Hmrdp` 自检一遍。
  `libhmrdp.so` 链接带 `-Wl,--no-undefined`（OHOS 工具链默认），它的漏导出在链接期就会失败，不必等到设备。

**音频后端替换**

- OHOS 的 OpenSL ES 已废弃且开设备不稳定 ⇒ FreeRDP **只解码**，把 16-bit PCM 交给 `libhmrdp` 注册的
  sink 及其交接环（见 §5、[`audio.md`](audio.md)）。
- **客户端侧缓冲/丢弃只有交接环一套策略**：`rdpsnd_main.c` 自带的 overrun 守卫（用墙钟反推设备积压，
  阈值 ≈ 1 个包）看不到环的真实深度，只会丢掉环还能播的包 ⇒ 本后端下把它置为"从不判 overrun"（patch step 35）。
  它不做任何缓冲/节流决策，只解码 + 转发。要动这层先读 [`audio.md`](audio.md) §3。

**运行时行为钩子**（都用**弱符号**引用，未打补丁的 FreeRDP 上自动降级）

- **触屏帧间隔**（patch step 06）：去掉上游 ~20ms 的固定合帧门限，rdpei 每次唤醒即发；速率由 ArkTS 的
  合帧窗口决定（见 [`session-layer.md`](session-layer.md) §8）。
- **合并后的触点帧必须仍是合法状态序列**（patch step 26）：一帧里每个触点只出现一次，所以应用在一次唤醒
  窗口内产生的 Down/Move/Up 会被合并进同一条记录。上游只保留最新的 flags，于是
  「DOWN 之后同一窗口内又来一个 Move」会让远端看到的首条记录是 `UPDATE|INRANGE|INCONTACT`——而进入
  ENGAGED 的合法迁移**只有** `DOWN|INRANGE|INCONTACT`（状态表在 `channels/rdpei/client/rdpei_main.h`）。
  远端因此从不 engage 这个触点：长按在那边成不了 press-and-hold（**不出右键菜单**），它为该畸形触点画下的
  触摸圆点还会残留，后续按压继续叠加。合并保留、只把**发出的记录**改成总是合法：DOWN 尚未发出的触点一律
  按 DOWN 报（坐标/字段取最新，合并不丢内容），在此之前到来的抬指留到下一帧发 UP；已 ENGAGED 的触点沿用旧
  行为（按住发 UPDATE、抬起发 UP）。"DOWN 已发出"这一标记加在 **channel 私有的** `rdpei_main.h` 结构里，
  不动公开头文件 ⇒ 不需要重新 sync。**判据**：只要远端收到的某触点首条记录不是 DOWN，就会在那边留下
  一个不消失的触点视觉。
- **释放那一帧不能移动触点**（patch step 28）：`rdpei_main.h` 的状态表写明「离开 ENGAGED
  （`UP`→OUT_OF_RANGE）时触点位置不能变，只能等迁移之后再变」。上游 `freerdp_handle_touch_up` 正是靠
  「先发一条 lift 位置的 `UPDATE`、再发同位置的 `UP`」来满足它（位置变发生在仍是 ENGAGED 的那条里）。但
  一帧只带一个触点（step 26），两条落进同一帧就只剩 `UP`，于是**位置变化被并进迁移那一帧**⇒ 远端不认这条
  释放、触点停在 ENGAGED（**触摸视觉不消失**）。是否落进同一帧取决于两条之间会不会插进一次 flush，所以
  故障是间歇的、且任何扰动时序的改动都会让出现率漂移。合并照旧，只是让 `UP` 报**远端已知的那个位置**
  （每触点记住上次上报的坐标，`lastX/lastY`），迁移就不移动触点；没发过 DOWN 的触点在 step 26 里已先补
  DOWN，它缓发的那条 `UP` 同样用这个位置。**判据**：RDPEI 发出的 `UP` 坐标必须等于该触点上一次上报的
  坐标（日志里就是 `touch tx ... flags=0x4 x=.. y=..` 与最近一条同坐标）。
- **解码宽度**：app 导出 `HmrdpDecodeWidth()`（`hmrdp_parallel.*`，值来自 `hmrdp_decode_tuning.*`：
  在线核数，上限 16，**不是设置项**），解码器在每条 region 边界读它。宽度是"这条 region 能用几个线程"：
  调用线程自己跑一个 chunk（调用方参与），平台队列的 `max_concurrency` 是宽度 − 1；没有平台执行器的构建
  读到 1 ⇒ `progressive.c` 走**完全串行**分支（不提交、不唤醒、不等待）。这里**没有 WinPR 池控制面**。
- **解码侧不设 dev 探针**：解码路径上只保留功能代码——分相计时、逐 tile 采样、逆 DWT 对拍这一组都不保留
  （连同其门控）。要重新量解码内部，只能按 [`rdp-gfx.md`](rdp-gfx.md) §7.1 的口径加一次性探针，量完就删。

**采集 / 回放 / QoS**

- **GFX 原始码流采集 + 离线回放钩子**：把服务端原始（仍 ZGX 压缩的）通道字节交给 `libhmrdp`
  （运行期回调，因为 DVC 插件比 NAPI 模块先加载，弱符号解析不到），并提供离线重放入口
  （`HmrdpGfxReplayNew*` / `HmrdpGfxReplayRecv`，其中 `…WithContext` 绑定到调用方自己的 `rdpContext`，
  供离线 gdi 桌面使用）。**这一整块按"一次性整体打补丁"设计**：改动它要从干净源码重打。
- **GFX 帧流水线独立成线程**（patch step 33，`HmrdpSetGfxDataSink`；理由见
  [`architecture.md`](architecture.md) §4）：补丁给 rdpgfx 数据入口加 sink 钩子——注册后把整块通道数据
  交给 app 并**立即返回**，app 在自己的线程上经 `HmrdpGfxReplayRecv`（离线回放同一入口）重新进入
  `rdpgfx_process_data`，drdynvc 线程因此只剩"拆包 + 拷贝"。不注册 sink 时行为不变；离线回放直接走
  `rdpgfx_process_data`，绕过 sink。
  **生命周期**：worker 在 **`HmrdpPostConnect`**（连接建立后）才启动——连接过程中的失败会在
  `freerdp_connect` 内部拆通道，早启动会与 worker 抢 rdpgfx 插件的生命周期；停止则必须在
  **`freerdp_disconnect` 之前** join（它释放该插件；offload 后 worker 与通道拆除不同线程，必须先把
  worker 停掉），随后才 `GfxWorkUninstall`。`Teardown`/`HmrdpPostDisconnect` 里各再兜一次。
- **客户端侧带宽 / 帧回执 / QoS**：**不动接收窗口**——上游只保证 ≥32K、其余交给内核自适应，显式放大
  `SO_RCVBUF` 会关掉自适应（实测同负载下到达速率约减半），所以接收窗口虽是高 RTT 链路的吞吐上限，
  却无法在客户端侧修。RDPGFX 的帧回执挪到 `EndFrame` 回调**之前**（否则本地上屏延迟整个落在服务端的
  每帧往返里）；app 注册的 **QoS** 钩子由 drdynvc 线程和 **GFX 工作线程**各自在入口调用一次（整条帧
  流水线现在在工作线程上）。解码的 tile worker 不来自 WinPR 池，它们的 QoS 由平台队列的任务属性给
  （见 [`rdp-gfx.md`](rdp-gfx.md) §7.3）。

**正确性 / 一致性**

- **Progressive tile 的预测器状态显式清零**：`progressive_tile_new` 里 `tile->current` / `tile->sign`
  直接来自 `malloc` 且**从未清零**，而 DIFFERENCE / UPGRADE 是**先读后写**的预测器状态；编码器假定
  客户端状态初值为 0，于是参考解码器的输出取决于堆里恰好有什么（对拍**不可复现**）。补丁在分配后
  `memset` 为 0。这条只让参考侧确定，不改协议语义。
- **CPU（gdi）链路的 progressive 解码调优**：tile 任务**分片 + 共享计数器动态领取**（替代每 tile
  一个线程池任务）、`update_tiles` **不逐 tile 建 `REGION16`**（一次取裁剪表 + 普通求交 + per-tile
  stamp 去重）、  `generic_image_copy_bgrx32_bgrx32` 的 keep-dst-alpha 拷贝采用**每像素一个掩码 32 位字**。
  三处都**不改变结果**（像素逐个相同、脏区面积相同）。整块按"一次性整体打补丁"设计：
  **改动它要从干净源码重打**。口径见 [`rdp-gfx.md`](rdp-gfx.md) §7.1。
- **并行执行器 = 平台队列（ffrt），唯一**：patch step 21 让解码在提交 chunk 前看弱符号
  `HmrdpParallelAvailable/Run`（由 app 的 `hmrdp_parallel.*` 提供）；没有这些导出时宽度读作 1 ⇒ 串行，
  **没有 WinPR 池兜底**。patch step 25 进一步在平台执行器可用时把 `rfx.c` 的 `UseThreads` 置 FALSE，
  不为解码建 WinPR 池。见 [`rdp-gfx.md`](rdp-gfx.md) §7.3。
- **tile 持久缓冲是 surface 级 arena**：patch step 22 把 `sign`/`current`/`data` 收成 surface 一整块、
  **按 tile 连续且 cache line 对齐**（缓冲内部的分量偏移不变，像素逐位相同；`HMRDP_TILE_ARENA` 是给 A/B
  用的编译期开关）。见 [`rdp-gfx.md`](rdp-gfx.md) §7.3。
- **tile 工作集来自裁剪矩形、合成收进共用 helper**：patch step 22b 给 tile 记下解码它的帧
  （`hmrdpFrameId`），`update_tiles` 由此只走**裁剪矩形覆盖到的 tile 范围**，不每条消息重走整帧累积的
  tile 列表；逐 tile 的裁剪求交 + 拷贝 + 脏区 span 记账收进 `hmrdp_composite_tile`，与 tile 解码侧的直写
  共用同一套几何。结果（tile 集合、裁剪、像素、脏区）不变。见 [`rdp-gfx.md`](rdp-gfx.md) §7.3。
- **tile 合成（拷贝）移进并行段**：patch step 23 把目标缓冲与合并后的 clip 存进 codec context，tile 解码
  完一块就直写 surface；`update_tiles` 保留遍历与 O(1) 脏区 span 记账，只在**clip 哈希一致**时跳过那次
  拷贝（哈希折入消息序号；"一条消息多条 region"时两者 clip 不同，仍由 `update_tiles` 覆盖）。
  `HMRDP_WORKER_TILE_COPY` 是 A/B 开关。见 [`rdp-gfx.md`](rdp-gfx.md) §7.3。
- **逆 DWT 改写**：Progressive 实际跑的是**抽取（外推）**那一支（`progressive_rfx_idwt_x/_y`，
  `RFX_DWT_REDUCE_EXTRAPOLATE` 区域），`codec/rfx_dwt.c` 的通用实现在全屏码流上一次也不进。抽取支与
  通用支**都只动组织、不动算术**：`X2 = L - (H0+H1)/2` 整段先算、输出对由无依赖的循环写，`idwt_y`
  按行主序组织；通用支另有一份 NEON（`VRHADD`/`VHADD` 是全精度半加，与 C 的 `int` 算术逐位相同）。
  注意抽取支的分母是**截断除 2**，不能写成 `>>1`。**这些是逐位等价的改写：`参考:对比` 的 `bad=0`
  仍然直接可用。**
- **允许舍入的那部分（`-DWITH_SIMD=ON`）**：抽取支的上游 NEON（8 路 16 位）、量化移位、`yCbCrToRGB`
  与部分 primitives 由 FreeRDP 自己的实现接管；非抽取支的上游 NEON **不接**（16 位车道相加会回绕，
  不是舍入）。这类改动由**量级门禁**兜底，见 [`rdp-gfx.md`](rdp-gfx.md) §7.1/§7.2。
- **`state` 只搬一趟（逐位等价）**：RLGR 解码**直接写 `sign`**（持久"原始"系数状态），去量化只做
  **`sign → buffer`**：没有"就地改 `buffer` + 另拷一份到 `sign`"的第二趟，每 tile/分量省掉 8KB 搬运，
  两个缓冲最终内容不变（**逐位等价 ⇒ 参考对比直接复用**）。LL3 是例外：差分解码要看"移位前"的值，
  所以它那 64/81 个样本先拷过去、再就地移位。`state` 绝对量约为逐 tile 独立缓冲时的一半。
- **桌面镜像 surface 直接合成进 primary 缓冲**：全屏 GFX 会话只有**一个** surface、映射到 `(0,0)`
  1:1、格式/行距与桌面相同 ⇒ 它就是桌面，`gdi_OutputUpdate` 的逐矩形 `freerdp_image_scale` 只是白搬一遍
  （~1ms/帧量级）。这一步把这个 surface 的 `data` 直接指向 `gdi->primary_buffer`（不自己 malloc），
  解码器写的就是呈现器要上传的内存，拷贝消失。判定凭证是 `surface->data == gdi->primary_buffer`；
  不满足就退回原路径。**生命周期是重点**：`gdi_ResetGraphics` 保留 surface 并 memset 它，而它的
  `DesktopResize` 会换掉 primary ⇒ 必须**换之前**记住谁在共享、**换之后**重新指向或让它自己分配，
  否则是"向已释放内存 memset"；`gdi_DeleteSurface` 不能释放共享缓冲；出现第二个 surface 时先解除共享。
  全部落在 `libfreerdp/gdi/gfx.c`，**不动头文件也不动 app**。详见 [`rdp-present.md`](rdp-present.md) §2。
- **encode-only 的 SIMD 桩不接、也不打警告**：上游 `nsc_neon.c` 的 `nsc_init_neon` 是空实现，只会刷
  `TODO: Implement neon optimized version of this function`；它对应的是 encode 侧（客户端只解码）⇒
  移除该警告（patch step 29）。

## 4. 编 FreeRDP 时的关键选项

- **`-DWITH_VERBOSE_WINPR_ASSERT=OFF` 必须保持**：默认 ON 时 `WINPR_ASSERT` 会 `abort()` 整个进程，
  任何后端健全性检查失败（如音频设备打不开）都会闪退；关闭后退化为被 `NDEBUG` 禁用的 `assert()`，
  错误走正常降级分支。
- `-DWITH_LIBRARY_VERSIONING=OFF`（见 §3）。
- `-DOPENSSL_CRYPTO_LIBRARY` / `-DOPENSSL_SSL_LIBRARY` / `-DZLIB_LIBRARY` 指向 `native/install/<arch>/*/lib`
  下的 `.so`，并置 `OPENSSL_USE_STATIC_LIBS=OFF`（moonlight 步同样如此）。winpr / freerdp / moonlight 都只是
  这些库的**消费者**，静态会把同一份 OpenSSL 复制到每个 `.so` 里（见 §1）。
- `-DWITH_SIMD=ON`：FreeRDP 使用**自己的 NEON 实现**（抽取支逆 DWT、量化移位、`yCbCrToRGB`、部分
  primitives），它们**不保证**与通用 C 逐位相同——抽取支 DWT 的 `(a+b+1)>>1` 在 16 位车道上会回绕，
  只是本工程码流上从未触发。所以"与 FreeRDP 逐像素一致"的口径是**量级**：由参考对比的 `rgbPx/maxDelta`
  给，且只在**同一构建配置**下可比（详见 [`rdp-gfx.md`](rdp-gfx.md) §7.1/§7.2）。
  **换这个开关必须重录参考画面**：`bad=0` 只表示"自那次重录起没有再变"。
- 优化等级：原生代码**一律 -O2**。预编译依赖由 `build-native.ps1` 以 `RelWithDebInfo` 构建（与 `Release`
  同码，多一份打包前被 strip 的 `-g`）；`libhmrdp.so` 自身源码（由 hvigor 按构建模式重编）在
  `CMakeLists.txt` 里把 debug 档覆盖成 `-O2 -g`——**`-O0` 不做保留**。判据：
  `entry/.cxx/**/debug/**/compile_commands.json` 里 `libhmrdp` 的编译命令应含 `-O2` 而不含 `-O0`。
  打包时 hvigor 的 `DoNativeStrip` 会 strip 所有 `.so`，HAP 内不含调试信息。
- FreeRDP 的安装头里 `winpr/build-config.h` 的 `WINPR_INSTALL_*` 保持**中性值**（安装后由 `build-native.ps1`
  同步时自动归一化）——不要把带构建机绝对路径的版本抄进仓库。

## 5. RDP 音频：rdpsnd sink 侧

两协议共享的播放链路（交接环、缓冲策略、渲染器 / OHAudio、格式变化与能力探测）见 [`audio.md`](audio.md)；
这里只写 FreeRDP / rdpsnd 侧。

- **sink 是一个注册进 FreeRDP 的 C 函数指针**：`HmrdpSetAudioSink` 注册的后端在 FreeRDP 的 async 播放线程
  （`rdpsnd->async` 下的 `play_thread`）上被调用，且必须**同步返回**该包的渲染延迟（ms，进 Wave Confirm）
  ——与文件上传 serve 同一类约束（[`architecture.md`](architecture.md) §3.1 判据 2）。它只把 PCM 写进共享的
  **有界 PCM 环**（`AudioSink`，`core/hmrdp_audio.*`），不做缓冲 / 丢弃 / 节流。
- **关闭 rdpsnd 自带的 overrun 守卫**（patch step 35）：它用墙钟反推设备积压、阈值 ≈ 1 个包，看不到环的
  真实深度，只会丢掉环还能播的包 ⇒ 本后端下置为"从不判 overrun"。要动这层先读 [`audio.md`](audio.md) §3。
- **格式变化**：sink 检测到 rate/channels 变化时清空旧格式队列，并让会话发 `AudioFormat` 事件
  （`kAudioFormat`）；ArkTS 据此（重新）建渲染器（[`audio.md`](audio.md) §4）。
- **播放侧与线程**：环的读取在 ArkTS 音频 worker（默认）或原生 OHAudio 回调线程，sink 回调在 FreeRDP
  播放线程，两者只共享环的锁（[`architecture.md`](architecture.md) §4.2/§4.4）。
