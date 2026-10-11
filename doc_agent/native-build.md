# 原生库构建体系（两协议共享）

应用的原生依赖**全部从源码交叉编译**，不使用系统预编译库。仓库**不提交**任何 `.so`。
本文件是这套**构建体系**的家：产物为何不入库、单一构建入口、依赖布局与自检、补丁体系；
各依赖自己的补丁内容与运行期事实见 [`rdp-native.md`](rdp-native.md)（FreeRDP / 音频 / 能力）与
[`moonlight-native.md`](moonlight-native.md)（moonlight-common-c）。

## 1. 为什么 `native/third_party/`、`entry/libs/<abi>/`、`entry/src/main/cpp/thirdparty/` 都不入库

- 源码/中间产物是数百 MB 的下载与构建输出；
- **产物里带着构建机的绝对安装路径**（`CMAKE_INSTALL_PREFIX` 会被编进 winpr 等库、也会写进生成的
  `winpr/build-config.h`）与本地 git 版本，属本机环境信息。

因此：**源码由脚本按固定版本拉取**（`native/scripts/fetch-sources.ps1`），构建产物放到 `entry/libs/<abi>/`；
应用编译用的 FreeRDP/winpr 头文件由 `build-native.ps1` 同步生成到 `entry/src/main/cpp/thirdparty/freerdp/`
（同步时归一化掉构建机路径与本地 git 版本）；Moonlight 的 `Limelight.h` 与 opus 头同步到
`thirdparty/{moonlight,opus}/`。`entry/src/main/cpp/CMakeLists.txt` 按
`${HMRDP_PREBUILT_LIBS}/libX.so` 完整路径链接、按 `thirdparty/freerdp/include/{freerdp3,winpr3}` 找头文件。

**每个依赖只以共享库存在一份，统一落 `entry/libs/<abi>/`**：共享还是静态的判据是**同一进程里是否存在多个
独立消费者**，不是"能不能共享"。`libcrypto`/`libssl` 的消费者有三个（`libwinpr3.so`、`libfreerdp3.so`、
`libmoonlight-common-c.so`），各自静态就必然有三份副本，而且两份以上还会各自导出同名全局符号、在查找时
互相抢占；zlib 与 opus 各只有一个消费者，同样打成共享库是为了口径统一——每个库都对应一个 `DT_NEEDED` 条目
和一个打包文件，不出现"这份归谁"的问题。**判据落在"独立依赖"这一层**：`moonlight-common-c` 自己的 submodule
（`enet`、`nanors`）不是独立依赖而是它的实现细节，照旧静态编进 `libmoonlight-common-c.so`，单独成 `.so`
只会多一个 DSO 和一次加载、没有任何去重收益。opus（`libopus.so`）是 Moonlight 音频解码的独立依赖，消费者是
`libhmrdp.so`（moonlight-common-c 只分帧、不解码，不消耗 opus；见 [`moonlight-native.md`](moonlight-native.md) §3.3）。
`libhmrdp` 因此**不链任何静态归档**，也就不需要 `-Wl,--exclude-libs`，更不需要"静态归档必须排在
`libwinpr3.so` 之前"这类链接顺序约束。

**自检**：`llvm-nm -D --defined-only` 在 `libwinpr3.so` / `libfreerdp3.so` / `libhmrdp.so` /
`libmoonlight-common-c.so` 里**不应出现**任何 `EVP_*` / `X509_*` / `OSSL_*` 定义符号（它们只应出现在
`libcrypto.so`，`SSL_*` 只应在 `libssl.so`），同一量在 `libhmrdp.so` 上是 `U`；每个非平台 `DT_NEEDED`
条目都必须能在 `entry/libs/<abi>/` 里找到同名文件。`--no-undefined` 已开（OHOS 工具链对共享库的默认），
漏导出在链接期就会报错。

## 2. 构建流程

```
native/scripts/fetch-sources.ps1   # 按固定版本拉取源码（tarball + SHA256 校验）并自动打补丁
native/scripts/build-native.ps1    # 编全部原生依赖 + 同步到模块（单一入口，见下）
```

- **单一入口 `build-native.ps1`**：按依赖顺序编 zlib → OpenSSL → opus → FreeRDP → moonlight，全部用 CMake +
  Windows OHOS NDK；每一步的**共享库都拷进 `entry/libs/<abi>/`**（模块自动打包，应用按完整路径链接），
  只把头文件同步到 `entry/src/main/cpp/thirdparty/`。`-Arch`（默认 arm64-v8a；x86_64 为模拟器）、
  `-Clean`、`-Only <步骤>`（只编某一步，依赖须已装；改 FreeRDP 补丁后就是 `-Only freerdp -Clean`）。
  经 `pwsh -File` 调用时 `-Only` 收不到数组语法，多步要写成 `-Only @("zlib","freerdp")` 直接调脚本。
- **优化级别一律 -O2**：`build-native.ps1` 用 `CMAKE_BUILD_TYPE=RelWithDebInfo`（与 `Release` 同码，
  多一份打包前会被 strip 的 `-g`），OpenSSL 的 CMake 另把 `-O2` 钉死；`libhmrdp.so` 自身源码的 debug 档
  也在 `CMakeLists.txt` 里覆盖成 `-O2 -g`——`-O0` 产物里行号与变量都不可信，对本工程没有调试价值。
  于是 debug / release 的差别只剩 `-g`、`NDEBUG` 与导出面，代码体积不随构建模式变。
- **OpenSSL 走 CMake、不进 WSL**：OpenSSL 3.x 没有 CMake 构建，源码树只有 Perl 模板（`*.h.in`）与
  `build.info`，而 Windows 侧没有 Perl。做法是把 `perl Configure` 的输出**冻结**成一份快照
  （`native/openssl-cmake/generated/`：生成的公开头 + provider 的 DER 生成物 + `sources.cmake` 源码清单），
  再用 `native/openssl-cmake/CMakeLists.txt` 按生成的 Makefile 同等预算编译成**两个共享库**
  `libcrypto.so` / `libssl.so`（同样的源集、宏、include 路径；`libdefault` / `liblegacy` / `libcommon` 的
  对象并入 libcrypto）。
  - **共享化要补两类构件，都由 `CMakeLists.txt` 从源码树现读现生成**，不必重出快照、也不涉及 WSL/Perl：
    - **导出表**：冻结的 Configure 是 `no-shared` 配置，没有生成 version script。这里解析
      `util/libcrypto.num` / `util/libssl.num`（公开符号表，随 tarball）生成一个**匿名** version script
      （`global:` 列表 + `local: *`，不带版本节点，与 FreeRDP 的无版本 SONAME 约定一致），并配
      `-Wl,--undefined-version` 容忍按配置不存在的条目。**不要加 `-fvisibility=hidden`**：导出由 version
      script 的 `global:` 名单决定，hidden 会把名单内的符号也变成本地，反而断掉 libssl → libcrypto。
    - **libssl 的"共享专有源"**：共享构建下 OpenSSL 把 `crypto/packet.c`（WPACKET）、`record/tls_pad.c`、
      `s3_cbc.c` 也编进 libssl（`ssl/build.info` 里的 `SHARED_SOURCE[../libssl]=`）；静态构建里它们只在
      libcrypto。这些行同样现读，不硬编码，换版本时自动跟上。
  - `libssl.so` 通过 `DT_NEEDED` 引 `libcrypto.so`。两个库都 `-O2` 钉死，且 `--gc-sections` 与 `local: *`
    一起用——导出面收敛掉的代码是被真正回收，而不只是被隐藏。
  - 只有换 OpenSSL 版本时用 `native/scripts/export-openssl-snapshot.ps1 -Configure` 重生成快照并入库
    （内部跑 `build-openssl-wsl.sh` 的 `perl Configure` + `make`；随后 `export-snapshot.pl` 从 `configdata.pm`
    取权威源码清单与生成物）。**改版本必须重出快照**，`sources.cmake` 与生成头随之更新。
  - `include/openssl/configuration.h` 是唯一按 ABI 变化的生成头（`RC4_INT`），快照按 arch 分份
    （`generated/arch/<arch>/`）；`crypto/buildinf.h` 含构建机路径/flag，由 CMake 写中性值，不入快照。
  - 产物路径与 WSL 版一致（`native/install/<arch>/openssl/{include,lib}`，`lib` 下是 `.so`）。
    **头文件不同步进模块**：`entry/src/main/cpp/` 没有任何 `#include <openssl/...>`（OpenSSL 只被
    FreeRDP 与 moonlight-common-c 这两个独立库使用），只有 `.so` 拷进 `entry/libs/<abi>/`。
  - 归一化/加固差异：CMake 构建沿用 OHOS toolchain 默认（`-fstack-protector-strong -ffunction-sections`
    等）；`buildinf.h` 与 `OPENSSLDIR` 等按中性值写。
- 版本与校验和固定在 `fetch-sources.ps1`（FreeRDP 3.10.3 / zlib 1.3.1 / OpenSSL 3.0.15 / opus 1.5.2）。
  已存在的树直接跳过，`-Force` 才重拉；拉取后自动跑补丁脚本（`patch-freerdp.ps1`、
  `patch-moonlight.ps1`、`patch-zlib.ps1`、`patch-opus.ps1`）。FreeRDP 的补丁成体系地拆在 `patch-steps/`；
  moonlight、zlib 与 opus 各只有一个入口脚本（见 [`rdp-native.md`](rdp-native.md) §3、
  [`moonlight-native.md`](moonlight-native.md) §3.3）。
- ⚠ **hvigor 不会清理不再产出的原生库**：`entry/libs/<abi>/` 里删掉一个 `.so` 后，
  `entry/build/<product>/intermediates/{cmake,libs,stripped_native_libs}/` 里的旧副本仍在，并会被打进
  HAP（症状：包体不变、里面多一个不该存在的库）。改动**产出集合**（新增/移除/改名某个依赖）之后，
  先删 `entry/build`——或至少上面三个 intermediates 目录——再打包。
- `patch-freerdp.ps1` 也可单独重复跑（幂等）。
- **改过补丁后必须用 `build-native.ps1 -Only freerdp -Clean`**：重新解包的源码树带着 tarball 里的旧 mtime，
  ninja 会认为目标文件比源新而**整块跳过编译**——症状是构建"成功"、`-Clean` 之外只产出一两个 `.so`，
  或者改动在 `.so` 里看不到（用 `nm`/字符串搜一下就知道）。`-Clean` 同时清安装前缀。
- 补丁报错时会连**失败的正则片段**一起打出来（`patch-freerdp.ps1` 的 `Patch-Regex`），照它改锚点即可；
  锚点跨步骤咬合，改一个要顺带看后面哪一步还引用着它。
- `build-native.ps1` 的 freerdp 步装完后自动把应用编译要用的头文件刷新到
  `entry/src/main/cpp/thirdparty/`（该目录**不入库**，见 §1），并把 `.so` 拷进 `entry/libs/<abi>/`。

改 FreeRDP（含改补丁脚本）后的**完整循环**：

```powershell
./native/scripts/fetch-sources.ps1                        # 首次/换版本；已有的树会跳过
./native/scripts/patch-freerdp.ps1
./native/scripts/build-native.ps1 -Only freerdp -Clean     # 重编 + 同步头 + 放回 .so
# 然后 build_project（HAP 才会带上新的 .so）
```

## 3. 补丁体系

- 补丁脚本的每一步都要**幂等 + 可自检**（已打过的步骤跳过并提示）。源码树是可丢弃的：
  干净上游 + 全部步骤必须能还原出它 ⇒ 新增/修改一律走补丁步骤（`patch-steps/`），
  **不要在 `third_party/` 里直接改**，否则重拉源码后改动就丢了。
- 补丁**按步拆分**：`native/scripts/patch-steps/<NN>-<topic>.ps1`，入口脚本 `native/scripts/patch-freerdp.ps1`
  是**唯一的公共工具来源**（`Patch-File` / `Patch-Block` / `Patch-Regex` / `Patch-Regex-All` /
  `Copy-PatchData` / `Tabs`）与"按名顺序 dot-source"。步骤里**不要再定义这些函数**：dot-source 会覆盖
  入口的定义、并让报错指到错误的文件。`<NN>` 对应被改源码注释里引用的步号（`8` 含两块，`22b` 表示 tile
  工作集那一步）。加/改一步只动那一个文件；每个步骤**自带 marker**，重跑时打印 `already applied` 并跳过。
- **marker 必须只出现在该步骤自己插入的文本里**：若某个 marker 同时出现在上游或更早步骤的替换里，
  该步骤会在干净源码上被静默跳过（症状：树里缺这块，后续步骤找不到锚点而报错）。入口的 `-Trace`
  会打印当前步骤名，便于定位。
- 匹配按 **LF 归一化**进行、写入时恢复文件原有风格（tarball 是 LF，Windows checkout 是 CRLF），
  所以补丁不依赖检出风格。
  ⇒ **"`fetch-sources.ps1 -Force` 重拉 + 跑一遍补丁、确认每步都只报 already applied、且没有异常"本身就是自检**：
  拆分或改动丢了内容会在那里炸出来（`Patch-Block`/`Patch-Regex` 找不到锚点会直接 throw）。
  - ⚠ **"已打补丁的树"上重跑整表并不总是可行**：后来的步骤会替换掉先前步骤的锚点/标记（例如 22b 重写了
    21 插入的 chunk 回调），那一步再跑就会 `pattern not found`。要自检就重拉源码；只是**给已打过补丁的树
    补一个新步骤**时用 `patch-freerdp.ps1 -Only <NN>` 只跑那一步。
- 步骤要拷贝的数据文件（音频后端源码、cmake 助手）在 `native/patches/`，入口脚本以 `$PatchData` / `$Patches`
  传给步骤。
- 只改应用层（`entry/src/main/cpp/{binding,core}/*`、`.ets`）**不需要**重编 FreeRDP。
