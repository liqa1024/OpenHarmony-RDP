# Moonlight 剪贴板（基地版扩展，**手动触发**）

本文件是 Moonlight 剪贴板通道的家：协议形态、主机能力边界、原生/ArkTS 分工。**两协议共享的手动剪贴板抽象**
见 [`clipboard.md`](clipboard.md) §1；会话窗口与工具栏见 [`session-layer.md`](session-layer.md) §9；
Moonlight 原生桥接见 [`moonlight-native.md`](moonlight-native.md) §2.4；控制层 HTTPS 见 [`moonlight-control.md`](moonlight-control.md)。

## 1. 非原版能力，主机侧才是硬边界

- **原版 Sunshine / GFE 没有剪贴板通道**，标准 Moonlight 协议也没有。它是 **foundation-sunshine（基地版）的扩展**，且依赖
  **主机用户会话里的 GUI agent**（`sunshine-control-panel` 的 Rust 侧）：Sunshine 的 C++ 核心只做纯字节转发，真正读写主机
  剪贴板、编解码 payload 的是该 agent。agent 未运行时不广告能力位，本端据此隐藏入口。
- **客户端没有过不去的门槛**：`0x5508` 收发、`LiSendClipboardData`、回调都是我们 patch `moonlight-common-c` 加上的
  （与光标 `0x5509`、触控板同策）。**真正的硬限制只看主机支持哪些 kind**。
- **能力位**：DESCRIBE 的 `x-ss-general.featureFlags` 位 `clipboard_text = 0x04` / `clipboard_image = 0x08`，置位且已连接才显示入口。

## 2. 主机硬能力（决定我们能支持什么）

| 内容 | 方向 | 说明 |
|---|---|---|
| 文本 UTF-8 | 双向 | `kind=1`，单帧 payload ≤ ~65500 |
| PNG 图片 | 双向 | `kind=2`；主机只认 PNG |
| 复合（文本 + 图） | 双向 | 文本帧在前、图片帧在后，共享同一非零 `token` |
| 大内容（文本 / 图） | 双向 | `kind=3 REF`（JSON `{id,mime,size}`）→ HTTPS `/api/v1/clipboard/blob[/<id>]`，≤50MiB / TTL 60s |
| 富文本 HTML/RTF | ✗ | 无对应 kind；主机只读纯文本 |
| 文件（客户端→主机） | ✗ | `kind=4` 主机侧收到即丢；主机→客户端的文件 offer 是另一个功能（文件夹映射） |

- **无 enable / 握手**：发帧只要求 Sunshine + 加密控制流 v2（`0x5508` 只在 v2 表里）。

## 3. 线格式与原生 / ArkTS 分工

**帧 v1（LE）**：`u8 version=1, u8 kind(1=text/2=png/3=ref/4=file-offer), u32 token, u32 length, bytes payload`（头 10 字节）。
`kind=3` 的 payload 是 `{"id","mime","size"}`，字节走 HTTPS blob。

- **原生只搬字节**：补丁给 `moonlight-common-c` 加 `0x5508` 收发——接收在控制流线程**同步**回调，发送入口
  `LiSendClipboardData`（返回 0/-1/-2/-3/-4）。发送路径同时把控制流的载荷长度由 `short` 放宽为 `int`、加密明文缓冲超 256 字节改堆分配，
  否则 64KB 级帧会越界。
- **协议语义全在 ArkTS**：`services/MoonlightClipboardBridge`（帧编解码、REF/blob、复合 token 配对、回显抑制、能力声明）挂在
  共享的 `services/ClipboardController` 上；N-API 在回调里只拷贝 + `napi_call_threadsafe_function`，解析/blob 拉取/PNG 解码都在 ArkTS 异步。

## 4. 手动语义与传输

- 手动语义（点按钮才读/写本机、不监听、不自动写）与回显抑制同 [`clipboard.md`](clipboard.md) §1/§2，此处不重复。
- **推送模型**：主机随时推帧，本端只缓存最新条目（复合按 token 配对），点「复制」才写本机。
- **图片双向**：出站 `PixelMap`→PNG、入站 PNG→`PixelMap`，走共享 worker 编解码 op。
- **REF/blob 走 HTTPS + 配对 mTLS**，端口取 serverinfo 的 `HttpsPort`（默认 47984），复用控制层证书 pin 与客户端证书。
- **范围**：仅文本 + 单张 PNG；文件、富文本不支持（主机所限）。
