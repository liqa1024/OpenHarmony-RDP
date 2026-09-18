# 原生库（源码构建 / 补丁 / 音频）

应用的原生依赖**全部从源码交叉编译**，不使用系统预编译库。仓库**不提交**任何 `.so`。

## 1. 为什么 `native/third_party/`、`entry/libs/<abi>/` 都不入库

- 源码/中间产物是数百 MB 的下载与构建输出；
- **产物里带着构建机的绝对安装路径**（`CMAKE_INSTALL_PREFIX` 会被编进 winpr 等库），属本机环境信息。

因此：**clone 后先本地构建**，把产物放到 `entry/libs/<abi>/` 再编译应用。
`entry/src/main/cpp/CMakeLists.txt` 按 `${FREERDP_LIBS}/libX.so` 完整路径链接。

## 2. 构建流程

```
native/scripts/patch-freerdp.ps1     # 给 FreeRDP 源码打 OHOS/定制补丁（幂等，可重复跑）
native/scripts/build-zlib.ps1        # zlib 静态库（Windows NDK）；FreeRDP 静态链入
native/scripts/build-openssl-wsl.sh  # OpenSSL，在 WSL 中运行，驱动 Windows OHOS clang
native/scripts/build-freerdp.ps1     # FreeRDP 的 CMake 构建（Windows NDK）
```

改 FreeRDP（含改补丁脚本）后的**完整循环**：

```powershell
./native/scripts/patch-freerdp.ps1
./native/scripts/build-freerdp.ps1 -Arch arm64-v8a
Copy-Item native/install/arm64-v8a/freerdp/lib/*.so entry/libs/arm64-v8a/ -Force
# 然后 build_project（HAP 才会带上新的 .so）
```

- 补丁脚本的每一步都要**幂等 + 可自检**（已打过的步骤跳过并提示）：源码树是**本地长期存在**的，
  干净源码不一定有备份 ⇒ **改补丁脚本时要保证"重跑安全"**，不要依赖"从干净源码重打"。
- 补丁**按步拆分**：`native/scripts/patch-steps/<NN>-<topic>.ps1`，入口脚本
  `native/scripts/patch-freerdp.ps1` 只放参数、公共工具（`Patch-File` / `Patch-Block` / `Patch-Regex` /
  `Tabs`）与"按名顺序 dot-source"；`<NN>` 就是被改源码注释里引用的步号（`8` 有两块，`18` 试过又撤掉）。
  加/改一步只动那一个文件；每个步骤**自带 marker**，重跑时打印 `already applied` 并跳过。
  ⇒ **"跑一遍补丁脚本、确认每步都只报 already applied、且没有异常"本身就是自检**：拆分或改动丢了内容会
  在那里炸出来（`Patch-Block`/`Patch-Regex` 找不到锚点会直接 throw）。
- 步骤要拷贝的数据文件（音频后端源码、cmake 助手）在 `native/patches/`，入口脚本以 `$PatchData` 传给步骤。
- 只改应用层（`entry/src/main/cpp/*`、`.ets`）**不需要**重编 FreeRDP。

## 3. 补丁脚本在做什么（以及为什么必须保留）

**平台兼容**

- musl/OHOS 下 `pthread_cancel` 等不存在，需按平台屏蔽/替换。
- OpenSLES 标识符映射：OHOS 只有标准 OpenSLES 1.0.1 头，没有 Android 扩展。

**链接与符号**

- client/common 编成**共享库**（默认静态会导出不出 NAPI 侧需要的符号）。
- **无版本号 SONAME**：配合 `-DWITH_LIBRARY_VERSIONING=OFF` 产出**不带版本号的单一 `libX.so`**
  （避免在 Windows 上实体化成 `libX.so.3` 的重复副本）；运行时 `DT_NEEDED` 也是 `libX.so`。
- 上游在该库上开了 `-DOHOS_ALLOW_UNDEFINED_SYMBOLS=ON`，所以**导出符号打错/漏定义只有到设备 dlopen
  时才炸**（症状：`relocating failed: symbol not found` + `does not provide an export name …` +
  启动即退）——改完补丁用 `llvm-nm -D --undefined-only libwinpr3.so | findstr Hmrdp` 自检一遍。

**音频后端替换**

- OHOS 的 OpenSL ES 已废弃且开设备不稳定 ⇒ FreeRDP **只解码**，把 16-bit PCM 交给 `libhmrdp`
  注册的 sink（见 §5）。

**运行时行为钩子**（都用**弱符号**引用，未打补丁的 FreeRDP 上自动降级）

- **触屏帧间隔可调**：上游把接触点合并成约 50Hz 一帧，补丁导出运行时全局
  `HmrdpSetTouchFrameInterval`，设置项「触屏-高刷新率」开则传 0。
- **解码线程数可运行时控制**：导出 `HmrdpSetDecodeThreads(n)`（0 = 内置默认）、
  `HmrdpGetDecodeThreads()`、`HmrdpApplyDecodeThreads(PTP_POOL)`（改线程数时**只动解码自己的那个池**，
  且在 region 边界调用，不会撕掉在飞的 work item）与 `HmrdpGetDecodeThreadsStats()`（dev 读数）。
  池是**每个 codec 上下文一个**（`rfx.c`），创建时就按同一个解析器定尺寸 ⇒ "改设置 → 下一条会话/回放
  生效"；`n == 1` 在 `progressive.c` 里走**完全串行**分支（不提交、不唤醒、不等待）。

**采集 / 回放 / QoS**

- **GFX 原始码流采集 + 离线回放钩子**：把服务端原始（仍 ZGX 压缩的）通道字节交给 `libhmrdp`
  （运行期回调，因为 DVC 插件比 NAPI 模块先加载，弱符号解析不到），并提供离线重放入口
  （`HmrdpGfxReplayNew*` / `HmrdpGfxReplayRecv`，其中 `…WithContext` 绑定到调用方自己的
  `rdpContext`，供离线 gdi 桌面使用）。**这一整块按"一次性整体打补丁"设计**：改动它要从干净源码重打。
- **客户端侧带宽 / 帧回执 / 线程扇出**：收窗口按 BDP 设置（`tcp.c`，连接前）；RDPGFX 的帧回执挪到
  `EndFrame` 回调**之前**（否则本地上屏延迟整个落在服务端的每帧往返里）；winpr 线程池 worker 与
  drdynvc 线程在入口调 app 注册的 **QoS** 钩子。**线程池的扇出上限只约束兜底路径**——并行宽度现在由
  平台队列的并发度决定（见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §0）。

**正确性 / 一致性**

- **Progressive tile 的预测器状态显式清零**：`progressive_tile_new` 里 `tile->current` / `tile->sign`
  直接来自 `malloc` 且**从未清零**，而 DIFFERENCE / UPGRADE 是**先读后写**的预测器状态；编码器假定
  客户端状态初值为 0，于是参考解码器的输出取决于堆里恰好有什么（对拍**不可复现**）。补丁在分配后
  `memset` 为 0。这条只让参考侧确定，不改协议语义。
- **CPU（gdi）链路的 progressive 解码调优**：tile 任务**分片 + 共享计数器动态领取**（替代每 tile
  一个线程池任务）、`update_tiles` **不再逐 tile 建 `REGION16`**（一次取裁剪表 + 普通求交 + per-tile
  stamp 去重）、`generic_image_copy_bgrx32_bgrx32` 的 keep-dst-alpha 拷贝改成**每像素一个掩码 32 位字**。
  三处都**不改变结果**（像素逐个相同、脏区面积相同），并导出 `HmrdpProgStat[8]` 供 app 的 `prog`
  统计行做归因。整块按"一次性整体打补丁"设计：**改动它要从干净源码重打**。数字与口径见
  [`gfx-engine.md`](gfx-engine.md) §8.1/§8.2。
- **并行执行器换成平台队列（ffrt）**：patch step 21 让解码在提交 chunk 前先看弱符号
  `HmrdpParallelAvailable/Run`（由 app 的 `hmrdp_parallel.*` 提供：并发队列 + `max_concurrency`
  + 任务属性 + 逐 handle 等待）；构建里没有这些导出时回落到原 WinPR 池。**宽度就是设置里的
  「解码线程数」**——实测并发宽度严格等于设定值，而原池不遵守该值。见
  [`cpu-accel-plan.md`](cpu-accel-plan.md) §0/§4。
- **tile 持久缓冲改成 surface 级 arena**：patch step 22 把 `sign`/`current`/`data` 由"每 tile 三次
  malloc"改成 surface 一整块、**按 tile 连续且 cache line 对齐**（缓冲内部的分量偏移不变，像素逐位相同；
  `HMRDP_TILE_ARENA` 是给 A/B 用的编译期开关）。见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §2。
- **tile 合成（拷贝）移进并行段**：patch step 23 把目标缓冲与合并后的 clip 存进 codec context，tile 解码
  完一块就直写 surface；`update_tiles` 保留遍历与 O(1) 脏区 span 记账，只在**clip 哈希一致**时跳过那次
  拷贝（哈希折入消息序号；"一条消息多条 region"时两者 clip 不同，仍由 `update_tiles` 覆盖）。
  `HMRDP_WORKER_TILE_COPY` 是 A/B 开关。效果见 [`cpu-accel-plan.md`](cpu-accel-plan.md) §2。
- **dev 探针的两处修正**：`g_HmrdpSampleTile` 改成线程本地（否则多 worker 下 `prog2` 的相位总量无意义）；
  app 侧新增 `energy:` 行（每核 busy×f² 的能量代理 + `C1=Σbusy×f` 的 cycle 代理，数据源为
  `cpuidle` 空闲时间与 `time_in_state` 驻留）。见 [`gfx-engine.md`](gfx-engine.md) §8.3。
- **逆 DWT 改写**：Progressive 实际跑的是**抽取（外推）**那一支
  （`progressive_rfx_idwt_x/_y`，`RFX_DWT_REDUCE_EXTRAPOLATE` 区域），`codec/rfx_dwt.c` 的通用实现
  在全屏码流上一次也不进。抽取支与通用支**都**改成"只动组织、不动算术"：`X2 = L - (H0+H1)/2`
  整段先算、输出对由无依赖的循环写，`idwt_y` 从按列走改成行主序；通用支另有一份 NEON
  （`VRHADD`/`VHADD` 是全精度半加，与 C 的 `int` 算术逐位相同）。注意抽取支的分母是**截断除 2**，
  不能写成 `>>1`。**这些是逐位等价的改写：`参考:对比` 的 `bad=0` 仍然直接可用。**
- **允许舍入的那部分（`-DWITH_SIMD=ON`）**：抽取支的上游 NEON（8 路 16 位）、量化移位、
  `yCbCrToRGB` 与部分 primitives 由 FreeRDP 自己的实现接管；非抽取支的上游 NEON **不接**
  （16 位车道相加会回绕，不是舍入）。这类改动由**量级门禁**兜底，见
  [`gfx-engine.md`](gfx-engine.md) §8.1/§8.4。
- **dev 对拍（`HmrdpSetDwtCheck` / `HmrdpDwtCheckStat[3]`）**：`参考:对比` 那一轮按 1/16 采样，
  把**真正跑的那一支**（NEON 或 C）与上游标量参照（`HmrdpDwtReference` /
  `HmrdpDwtExtrapolateReference`）逐元素比，报 `dwt check: tiles=… mismatch=… maxDelta=…`。
  取输入副本必须在解码之前（DWT 会把整个系数缓冲覆盖掉）；`maxDelta` 是"舍入 vs 实现不同"的分界。
- **`state` 少一趟搬运（逐位等价）**：RLGR 解码**直接写 `sign`**（持久"原始"系数状态），去量化那趟改成
  **`sign → buffer`**——原先是"就地改 `buffer` + 另拷一份到 `sign`"，于是每 tile/分量的那趟 8KB 搬运
  整趟消失，两个缓冲最终内容不变（**逐位等价 ⇒ 参考对比直接复用**）。LL3 是例外：差分解码要看"移位前"
  的值，所以它那 64/81 个样本先拷过去、再就地移位。读数见
  （`state` 绝对量 −5 成）。
- **桌面镜像 surface 直接合成进 primary 缓冲**：全屏 GFX 会话只有**一个** surface、映射到 `(0,0)`
  1:1、格式/行距与桌面相同 ⇒ 它就是桌面，`gdi_OutputUpdate` 的逐矩形 `freerdp_image_scale`
  只是白搬一遍（~1ms/帧量级）。这一步把这个 surface 的 `data` 直接指向 `gdi->primary_buffer`
  （不再自己 malloc），解码器写的就是呈现器要上传的内存，拷贝消失。判定凭证是
  `surface->data == gdi->primary_buffer`；不满足就退回原路径。
  **生命周期是重点**：`gdi_ResetGraphics` 保留 surface 并 memset 它，而它的 `DesktopResize` 会换掉
  primary ⇒ 必须**换之前**记住谁在共享、**换之后**重新指向或让它自己分配，否则是"向已释放内存
  memset"；`gdi_DeleteSurface` 不能释放共享缓冲；出现第二个 surface 时先解除共享。
  全部落在 `libfreerdp/gdi/gfx.c`，**不动头文件也不动 app**。详见 [`present-pipeline.md`](present-pipeline.md) §4.5。

## 4. 编 FreeRDP 时的关键选项

- **`-DWITH_VERBOSE_WINPR_ASSERT=OFF` 必须保持**：默认 ON 时 `WINPR_ASSERT` 会 `abort()` 整个进程，
  任何后端健全性检查失败（如音频设备打不开）都会闪退；关闭后退化为被 `NDEBUG` 禁用的 `assert()`，
  错误走正常降级分支。
- `-DWITH_LIBRARY_VERSIONING=OFF`（见 §3）。
- `-DWITH_SIMD=ON`：FreeRDP 使用**自己的 NEON 实现**（抽取支逆 DWT、量化移位、`yCbCrToRGB`、部分
  primitives），它们**不保证**与通用 C 逐位相同——抽取支 DWT 的 `(a+b+1)>>1` 在 16 位车道上会回绕，
  只是本工程码流上从未触发。所以"与 FreeRDP 逐像素一致"的口径改成**量级**：dev 对拍报 `maxDelta`、
  参考对比报 `rgbPx/maxDelta`，且只在**同一构建配置**下可比（详见
  [`gfx-engine.md`](gfx-engine.md) §8.1/§8.4）。**换这个开关必须重录参考画面**：
  `bad=0` 只表示"自那次重录起没有再变"。
- 优化等级：`libhmrdp.so` 由 hvigor 按构建模式重编（debug → `-O0 -g`，release → `-O2 -DNDEBUG`），
  **不要在 `CMakeLists.txt` 里写死 `-O2`**，否则会覆盖 debug 的 `-O0`。FreeRDP 预编译库固定
  `-O2 -DNDEBUG`，**不随构建模式变**。打包时 hvigor 的 `DoNativeStrip` 会 strip 所有 `.so`，
  HAP 内不含调试信息。
- FreeRDP 的安装头里 `winpr/build-config.h` 的 `WINPR_INSTALL_*` 保持**中性值**（构建脚本安装后会自动
  归一化）——不要把带构建机绝对路径的版本抄进仓库。

## 5. 音频：FreeRDP 只解码，播放用原生 OHAudio

- `libhmrdp` 用 **`dlopen("libohaudio.so")` + `dlsym`** 解析 OHAudio API 建 `OH_AudioRenderer`
  （`writeData` 拉模型 + 环形缓冲丢帧）。**不要**把 `libohaudio.so` 链成 `DT_NEEDED`：缺库设备会
  加载即崩。也不要用 ArkTS `@ohos.multimedia.audio`（该工具链下会触发 syscap 误报，且 OpenSLES 已废弃）。
- **绝不持锁调用 `Start`/`Stop`/`Release`**：`OH_AudioRenderer_Release` 会等待 write 回调
  （`JoinCallbackLoop`），而回调要用同一把 `mutex_` 取环形缓冲 ⇒ 持锁 Release 必死锁
  （表现为关闭会话后 `APP_INPUT_BLOCK` 卡死）。约定：`mutex_` 只保护环形缓冲/句柄，**不跨 OHAudio
  调用持有**；`lifecycle_` 只在 `mutex_` 之外串行化 open/close；回调只碰 `mutex_`。
- 丢帧统计：`OnWrite` 欠载补静音 + 环形缓冲溢出的字节；**仅活跃时**统计
  （见 [`session-and-input.md`](session-and-input.md) §3）。

## 6. 设备能力探测（Capability 模式）

新增**与设备相关**的特性时，不要只靠 syscap 告警，也不要在不支持时静默失败：

- 原生侧优雅降级（探测失败 ⇒ 关掉对应 FreeRDP 特性，例如 `AudioOutput::Supported()` ⇒
  关闭 `FreeRDP_AudioPlayback`）；
- ArkTS 侧在 `services/DeviceCapabilities.ets` 暴露 `Capability{supported, reason}`；
- UI 上**置灰**该开关并显示原因。

已用此模式的：音频重定向、硬件加速。原生只回**稳定原因码**
（`no-vulkan`/`no-instance`/`no-device`/`no-host-memory`/`no-surface`/`emulator`；引擎另有 `no-compute`），
中文文案由 `DeviceCapabilities.hardwareAccel()` 负责；不支持时设置页置灰开关并显示原因，
并把已存的值纠正为关。

**两套能力判定，不要混用**（`FillVerdicts()`，`hmrdp_vk_context.*`）：

| 判定 | 用途 | 条件 |
|---|---|---|
| **引擎判定**（`engineSupported`） | GPU 引擎 / GPU 回放，最严 | device + **graphics+compute 队列** + host-visible 内存 + `VK_OHOS_surface`/`VK_KHR_swapchain`（Progressive 解码是 compute dispatch） |
| **呈现判定**（`presenterSupported`） | 「硬件加速」开关 / 上屏，较宽松 | device + host-visible 内存 + `VK_OHOS_surface`/`VK_KHR_swapchain`，**不需要 compute** |

- 呈现判定决定「硬件加速」是否有意义（`vulkanAccelSupport()` → 设置页置灰），以及
  `CreateFramePresenter()` 用 Vulkan 呈现器还是 GLES 兜底呈现器（开关关掉时也是 GLES）。
- **呈现判定实际对应的 Vulkan 面**（`VkRenderer` 用它，逐项可核）：instance 扩展
  `VK_KHR_surface` + `VK_OHOS_surface`，device 扩展**只有** `VK_KHR_swapchain`；一个
  graphics+present 队列族；swapchain（`R8G8B8A8_UNORM`，FIFO）+ render pass/framebuffer/view；
  一条 graphics pipeline（`present_quad.vert/frag`，动态 viewport/scissor 做 letterbox，
  sampler + descriptor set + push constant）；持久桌面 image（`B8G8R8A8_UNORM`）与 host-visible
  staging/零拷贝缓冲；command pool/buffer×2、二进制信号量、fence×2；`vkCmdCopyBufferToImage` +
  `vkCmdPipelineBarrier` + `vkCmdBeginRenderPass/Draw/EndRenderPass` + `QueueSubmit/Present`。
  **用不到**：compute（`CmdDispatch`/`CreateComputePipelines`）、`vkCmdBlitImage`、
  `vkCmdClearColorImage`/`CmdCopyImage`/`CmdFillBuffer`/`CmdCopyBuffer`、timeline 信号量、
  外部内存/原生缓冲导入（后两者只在探测里**报告**，没有任何调用点）。
  ⇒ 只要"能上屏"的设备就能走 Vulkan 呈现器；**硬件加速开关若只控制呈现后端，就该用呈现判定**。
- 两者都**排除模拟器包**（x86_64 构建）：模拟器会按标准接口谎报能力，能力探测排除不掉它；
  这与"GPU 只在真机验证"的口径一致，所以模拟器上呈现回落到 GLES。
- `native_window` 是 `libhmrdp` 显式链接的显示栈 API（把 XComponent 的 surfaceId 变成
  `OHNativeWindow`，交给 Vulkan/EGL）；`EGL`/`GLESv3` 只服务 GLES 兜底呈现器。都是设备必备库，
  不需要像 OHAudio 那样做缺库降级。
