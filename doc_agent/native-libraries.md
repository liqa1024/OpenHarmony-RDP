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

- 补丁脚本的每一步都要**幂等 + 可自检**（已打过的步骤跳过并提示），因为源码树是**本地长期存在**的，
  而干净源码不一定有备份：**改补丁脚本时要保证"重跑安全"**，不要依赖"从干净源码重打"。
- 只改应用层（`entry/src/main/cpp/*`、`.ets`）**不需要**重编 FreeRDP。

## 3. 补丁脚本里各步在做什么（以及为什么必须保留）

1. **musl/OHOS 兼容**：`pthread_cancel` 等在 musl 下不存在，需按平台屏蔽/替换。
2. **client/common 编成共享库**：默认静态，会导出不出 NAPI 侧需要的符号。
3. **OpenSLES 标识符映射**：OHOS 只有标准 OpenSLES 1.0.1 头，没有 Android 扩展。
4. **rdpsnd 后端替换**：OHOS 的 OpenSL ES 已废弃且开设备不稳定 ⇒ 后端不再自己开设备，只把解码后的
   16-bit PCM 交给 `libhmrdp` 注册的 sink（见 §5）。
5. **无版本号 SONAME**：配合 `-DWITH_LIBRARY_VERSIONING=OFF` 产出**不带版本号的单一 `libX.so`**
   （避免 Windows 上实体化成 `libX.so.3` 的重复副本）；运行时 `DT_NEEDED` 也是 `libX.so`。
6. **RDPEI 触屏帧间隔可调**：上游把接触点合并成约 50Hz 一帧。补丁导出运行时全局
   `HmrdpSetTouchFrameInterval`，`libhmrdp` 以**弱符号**引用；设置项「触屏-高刷新率」开则传 0。
   未打补丁的 FreeRDP 上弱符号为空，开关自动降级。
7. **GFX 原始码流采集 + 离线回放钩子**：把服务端原始（仍 ZGX 压缩的）通道字节交给 `libhmrdp`
   （运行期回调，因为 DVC 插件比 NAPI 模块先加载，弱符号解析不到），并提供离线重放入口
   （`HmrdpGfxReplayNew*` / `HmrdpGfxReplayRecv`，其中 `…WithContext` 绑定到调用方自己的 `rdpContext`，
   供离线 gdi 桌面使用）。**这一整块按"一次性整体打补丁"设计**：改动它要从干净源码重打。

## 4. 编 FreeRDP 时的关键选项

- **`-DWITH_VERBOSE_WINPR_ASSERT=OFF` 必须保持**：默认 ON 时 `WINPR_ASSERT` 会 `abort()` 整个进程，
  任何后端健全性检查失败（如音频设备打不开）都会闪退；关闭后退化为被 `NDEBUG` 禁用的 `assert()`，
  错误走正常降级分支。
- `-DWITH_LIBRARY_VERSIONING=OFF`（见 §3.5）。
- `-DWITH_SIMD=OFF`：**注意**：这会让 FreeRDP 使用**通用 C 实现**而不是 NEON/SSE 变体。某些算法
  （如渐进式解码的 IDWT）在不同实现下**舍入不同**，所以"与 FreeRDP 逐像素一致"这件事**只在同一构建
  配置下成立**（详见 [`gfx-engine.md`](gfx-engine.md)）。
- 优化等级：`libhmrdp.so` 由 hvigor 按构建模式重编（debug → `-O0 -g`，release → `-O2 -DNDEBUG`），
  **不要在 `CMakeLists.txt` 里写死 `-O2`**，否则会覆盖 debug 的 `-O0`。FreeRDP 预编译库固定 `-O2 -DNDEBUG`，
  **不随构建模式变**。打包时 hvigor 的 `DoNativeStrip` 会 strip 所有 `.so`，HAP 内不含调试信息。
- FreeRDP 的安装头里 `winpr/build-config.h` 的 `WINPR_INSTALL_*` 保持**中性值**（构建脚本安装后会自动
  归一化）——不要把带构建机绝对路径的版本抄进仓库。

## 5. 音频：FreeRDP 只解码，播放用原生 OHAudio

- `libhmrdp` 用 **`dlopen("libohaudio.so")` + `dlsym`** 解析 OHAudio API 建 `OH_AudioRenderer`
  （`writeData` 拉模型 + 环形缓冲丢帧）。**不要**把 `libohaudio.so` 链成 `DT_NEEDED`：缺库设备会
  加载即崩。也不要用 ArkTS `@ohos.multimedia.audio`（该工具链下会触发 syscap 误报，且 OpenSLES 已废弃）。
- **绝不持锁调用 `Start`/`Stop`/`Release`**：`OH_AudioRenderer_Release` 会等待 write 回调
  （`JoinCallbackLoop`），而回调要用同一把 `mutex_` 取环形缓冲 ⇒ 持锁 Release 必死锁
  （表现为关闭会话后 `APP_INPUT_BLOCK` 卡死）。约定：`mutex_` 只保护环形缓冲/句柄，**不跨 OHAudio 调用持有**；
  `lifecycle_` 只在 `mutex_` 之外串行化 open/close；回调只碰 `mutex_`。
- 丢帧统计：`OnWrite` 欠载补静音 + 环形缓冲溢出的字节；**仅活跃时**统计（见 [`session-and-input.md`](session-and-input.md) 的遥测）。

## 6. 设备能力探测（Capability 模式）

新增**与设备相关**的特性时，不要只靠 syscap 告警，也不要在不支持时静默失败：

- 原生侧优雅降级（探测失败 ⇒ 关掉对应 FreeRDP 特性，例如 `AudioOutput::Supported()` ⇒
  关闭 `FreeRDP_AudioPlayback`）；
- ArkTS 侧在 `services/DeviceCapabilities.ets` 暴露 `Capability{supported, reason}`；
- UI 上**置灰**该开关并显示原因。

已用此模式的：音频重定向、**硬件解码（RFX）**。**待补**：GPU 回放入口的置灰
（硬件加速是"真机专属"，见 [`gfx-engine.md`](gfx-engine.md)）。

硬件解码的判据由原生侧给出（`vulkanEngineSupport()`，取自 `VulkanCapabilities::engineSupported`）：
设备可用 + 有 **graphics+compute** 队列族（Progressive 解码是 compute dispatch）+ 有 **host-visible** 内存
（表面/缓存是常驻映射缓冲）+ 有 **VK_OHOS_surface 与 VK_KHR_swapchain**（上屏）；另外**模拟器包
（x86_64 构建）一律判定不支持**——模拟器会按标准接口谎报能力，能力探测排除不掉它，这与
"GPU 只在真机验证"的口径一致。原生只回**稳定原因码**（`no-vulkan`/`no-instance`/`no-device`/
`no-compute`/`no-host-memory`/`no-surface`/`emulator`），中文文案由 `DeviceCapabilities.hardwareDecode()`
负责；不支持时设置页置灰开关并显示原因，并把已存的值纠正为关。
