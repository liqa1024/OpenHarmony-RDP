# 剪贴板（手动触发，两协议共享抽象）

本文件是**两协议共享的剪贴板机制**的家：手动语义、共享控制器 / 桥 / 本机读写叶子与用户文案。
RDP 侧的线格式（cliprdr 协商、DIB/CF_HTML/RTF/PNG）与文件传输见 [`rdp-clipboard.md`](rdp-clipboard.md)；
Moonlight 侧协议见 [`moonlight-clipboard.md`](moonlight-clipboard.md)；工具栏按钮的显示与自动隐藏见
[`session-layer.md`](session-layer.md) §9；原生边界判据见 [`architecture.md`](architecture.md) §3.1。

## 1. 共享抽象

剪贴板的手动读写机制是**协议中立**的，抽在 `services/` 与 `utils/`，RDP 与 Moonlight 共用一份：

- `services/ClipboardController`：**手动语义与编排**——远端条目缓存、`复制`（写本机）、`粘贴`（读本机后交给桥），
  以及文件传输的单传输约束、进度、取消、沙箱目录保留策略。它只驱动 `bridge.capabilities()` 为真的路径。
- `services/ClipboardBridge`：协议缝。`capabilities()` 声明本协议能带哪些内容（`text/html/image/files`）；`sendItem`
  由桥按自身能力择一发送；`remoteItem` 交回缓存的远端内容；传输状态经 sink 回调上抛给控制器。
- `utils/LocalPasteboard`：本机系统剪贴板读写（文本 / HTML Entry / 图片 / 文件 URI）与 HTML 文本工具，协议无关。
- 图片中立表示 = `image.PixelMap`；`PixelMap↔PNG` 编解码是共享 worker op（`ENCODE_PNG`/`DECODE_PNG`）。
- 共享 UI：`components/SessionChrome` 的 `ClipboardButtons` / `ClipboardTransferStatus`，两协议工具栏复用。

**能力降级**：RDP 桥声明全能力（文本/HTML/图片/文件），协议机制（cliprdr 协商、DIB/CF_HTML/RTF 线格式、文件流、
沙箱目录命名）留在 `services/RdpClipboardBridge`；Moonlight 桥只声明 `text + image`（见
[`moonlight-clipboard.md`](moonlight-clipboard.md)），文件/富文本分支永不被触及。

**用户文案**：共享在 `model/ClipboardModels.ets` 的 `ClipboardText`，遵循 [`ui-design.md`](ui-design.md) §4.3
（结论 + 具体对象、失败用「无法 X：原因」、不加句末标点）。**动词与工具栏按钮一致**（粘贴 = 本机→远端，复制 = 远端→本机），
且**必须点名内容类型**（文本 / 富文本 / 图片 / 文件）——如「已粘贴图片到远端」「无法粘贴图片到远端」「已复制 3 个文件到本机」。
两协议读同一份，避免各写一套。

## 2. 设计原则：只手动同步

**同步在设计上就是用户手动触发的**：工具栏「复制」（远端→本机）与「粘贴」（本机→远端），
**刻意不做自动同步**，扩展图片/文件等类型时也必须沿用。理由：

1. 自动读本机剪贴板需要受限开放的读权限、有隐私成本；手动则由系统 `PasteButton` 安全控件在运行时临时授权，
   读取无需声明 `READ_PASTEBOARD`；
2. 图片/文件等大数据量只有显式触发才可控（二次确认、进度、目标路径）；
3. 两个方向逻辑对称（都是"读一侧 → 写另一侧"），扩展类型只需改中间那段转换。

手动读取不声明 `READ_PASTEBOARD`，但 `getData` / `getDataWithProgress` 仍会被编译器**静态**提示该权限
（授权是运行时由安全控件给的）；两个读取方法用 `@SuppressWarnings({rules:[SuppressWarningsType.PERMISSION]})`
压掉该告警。

**不要引入剪贴板自动监听或自动写本机剪贴板。**
