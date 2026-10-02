# Moonlight 原生客户端

本文件是 **Moonlight / Sunshine 串流的原生侧**知识：客户端库的拉取/补丁/构建与静态链接、桥接层约定，
以及后续的配对、应用列表、串流会话、输入与上屏接线。**前端**侧（协议建模、存储布局、连接列表与编辑页、
设置页分栏、图标）见 [`moonlight-connection.md`](moonlight-connection.md)；RDP 的 FreeRDP/OpenSSL/zlib
构建见 [`native-libraries.md`](native-libraries.md)（本文件不重复它）。

## 1. 客户端库（moonlight-common-c）的构建与链接

- **客户端库选型**：`moonlight-common-c` 是各 Moonlight 客户端共用的核心（配对、HTTP 应用列表、RTSP 会话
  协商、control/audio/video 流的分帧）。上游**没有 release tag**，按 commit 固定。
- **依赖树**：它的 `enet` 与 `nanors` 是 **git submodule**，commit tarball 不含；`fetch-sources.ps1` 分别按
  commit 固定拉取后放进 `moonlight-common-c/{enet,nanors}`（它的 CMake 用 `add_subdirectory(enet)` 并在
  `nanors/rs.c` 处编译）。enet 上游恒为静态库。
- **构建命令**（在 FreeRDP 那套之后）：

  ```
  native/scripts/fetch-sources.ps1     # 拉取并按 commit 固定；自动跑 patch-moonlight.ps1
  native/scripts/build-moonlight.ps1   # Windows NDK 交叉编译，静态归档
  ```

- **静态链接，不单独打包**：与 FreeRDP 不同，它没有运行时加载插件、不需要导出符号，因此构建成静态库
  （`-DBUILD_SHARED_LIBS=OFF`），连同 enet、OpenSSL 的 `libcrypto.a` 一起链进 `libhmrdp.so`；
  `entry/libs/<abi>/` 里**不新增 `.so`**。两者都带 `-fPIC`。
- **归档与头文件位置**：`build-moonlight.ps1` 不能依赖 `cmake --install`（上游 CMakeLists 不写 install 规则），
  它把 `libmoonlight-common-c.a` / `libenet.a` / `libcrypto.a` 与公开头 `Limelight.h` 收集到
  `native/install/<arch>/moonlight/`，再由 `sync-moonlight.ps1` 同步到
  `entry/src/main/cpp/thirdparty/moonlight/{include,lib/<abi>}/`（生成目录，**不入库**，与 FreeRDP 头文件同策）。
  `sync-moonlight.ps1` 按 arch 只替换 `lib/<abi>`，两个 ABI 可共存。
- **OpenSSL 依赖**：只用 `libcrypto`（配对 AES），复用它已经静态构建的那一份；构建时显式传
  `OPENSSL_ROOT_DIR` / `OPENSSL_CRYPTO_LIBRARY` 等，使 `find_package(OpenSSL 1.0.2 REQUIRED)` 命中
  OHOS 版本而不是宿主。
- **平台层**：它自带 POSIX 平台实现（`Platform*.c`，`LC_POSIX` 分支），OHOS 直接可用；配置阶段的自检
  （`fcntl` / `ioctl` / `poll` / `getaddrinfo` / `gethostbyname_r` / `inet_pton` / `clock_gettime` 的
  `CLOCK_MONOTONIC`）全部命中。
- **补丁**：`patch-moonlight.ps1` 去掉上游在非 MSVC/MinGW 平台无条件加的 `-Werror`（它加在 `CMAKE_C_FLAGS`
  之后，命令行关不掉）。warnings 仍可见，只是不再中断构建。

## 2. 桥接层与降级

- **降级式接线**：`entry/src/main/cpp/CMakeLists.txt` 只在
  `thirdparty/moonlight/lib/<abi>/libmoonlight-common-c.a` 存在时才定义 `HMRDP_HAVE_MOONLIGHT` 并链接；
  缺归档时 `hmrdp_moonlight.*` 编译成"不支持"，构建照常（同 FSR / XEngine 的降级写法）。因此未跑原生构建的
  检出仍能 `build_project`。
- **静态库必须真被引用才算连上**：静态归档里没被引用的目标不会进 `.so`。`hmrdp_moonlight.cpp` 的探针
  真正调用一个 moonlight 入口（`LiInitializeStreamConfiguration`，纯字段初始化、不起线程/不建 socket），
  所以"符号已解析"由**链接器**保证：有未解析符号则模块链接失败。
- **N-API 探针**：`moonlightProbe` 回 `"1"`，或未构建该库时回 `"0|no-moonlight"`（稳定原因码，中文文案归
  UI 层，同 [`native-libraries.md`](native-libraries.md) §6 的能力探测模式）。

## 3. 当前边界与后续

- 已落地的是**前端**与**原生客户端库基础**：库已静态链入、探针可证符号解析。
- **尚未实现**：主机发现、**配对**、**应用列表**、串流会话建立、输入翻译与接入现有会话外壳/上屏。
  在此之前，列表/编辑页发起 Moonlight 连接只给提示，不建立会话。
- 路线与阶段（含"离线优先"的实施顺序）见 [`../MOONLIGHT_PLAN.md`](../MOONLIGHT_PLAN.md)。
- **改前必读**：会话侧接线要遵守既有的线程模型（动态通道分发线程只做轻活，帧重活走 sink 钩子），见
  [`architecture.md`](architecture.md) §4；上屏复用既有呈现器，见 [`present-pipeline.md`](present-pipeline.md)。
