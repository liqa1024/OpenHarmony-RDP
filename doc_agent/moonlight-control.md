# Moonlight 控制 / 配对层（ArkTS）

本文件是 Moonlight 路**控制 / 配对层**的家：`serverinfo / pair / applist / launch / resume / cancel` 的编排、
配对时序与客户端身份、证书 pin 与存储、控制端点参数名——**全部在 ArkTS**，原生不含这一层。
连接建模与存储见 [`moonlight-connection.md`](moonlight-connection.md)；主机与应用（运行态 / 唤醒 / 应用管理页）
见 [`moonlight-apps.md`](moonlight-apps.md)；会话窗口接线见 [`moonlight-session.md`](moonlight-session.md)；
原生库与桥接见 [`moonlight-native.md`](moonlight-native.md)；视频解码与协商见 [`moonlight-video.md`](moonlight-video.md)。

## 1. 落地与编排

传输与 mTLS / 证书 pin 用 `@ohos.net.http`，配对加密原语用 `cryptoFramework` 与 `@ohos.security.cert`，
客户端身份（自签名 X.509 + HUKS 信封加密私钥）在 ArkTS 组装；**原生不含这一层**。落地在 `ets/moonlight/`：
`MoonlightCrypto`（配对加密原语）、`MoonlightIdentity`（身份与 pin 存储）、`MoonlightHttp`（传输）、
`MoonlightClient`（`serverinfo → pair → applist → launch` 编排）与 `MoonlightXml` / `MoonlightModels`。
串流前预检由 `services/MoonlightConnector.ets` 编排（`serverinfo →`（未配对则先 `pair`）`→ launch`，并装配原生
串流配置），由 `MoonlightSessionBackend.prepare()` 在 `SessionManager` 已发布的 `Connecting` 状态下驱动，
故列表行加载态与 RDP 一致（见 [`session-layer.md`](session-layer.md) §5）。列表 / 编辑页 / 应用管理页的
「连接」统一经 `services/MoonlightLauncher.ets` 构造请求（解析分辨率、应用 id、`filesDir` 与配对 PIN 弹窗）
后交给 manager。

## 2. 配对：时序、身份与原语

- **配对 = 依赖主窗口的轻量弹窗**：客户端生成 4 位 PIN，弹窗只**显示** PIN（用户在主机上输入，四个数字居中放大），
  配对期间不开任何会话窗口；**主机端输入完成即握手结束，弹窗自动关闭并继续**（无需手动确认），只有「取消」是用户动作；
  配对完成后才 launch 并开会话。
- **信任模型 = 证书 pin**：未配对时接受任意主机证书（首触）；配对后存叶证书，之后逐字节一致。主机自签名，
  信任靠 pin 而非 CA。
- **端口**：serverinfo 与配对前三步走明文 **HTTP 47989**，`pairchallenge` 走 **HTTPS 47984**（mTLS）。
- **配对时序**：客户端先发起 `getservercert`——**主机正是收到它才挂起并提示输入 PIN**，所以客户端要
  **先发请求、再显示 PIN**（两者并行，不是"先等用户输入再发"）；随后
  `clientchallenge → serverchallengeresp → clientpairingsecret → pairchallenge`。
  `getservercert` 要等主机端输入完成，**无读超时**（其余 5s 量级）。同一轮配对的
  `uniqueid` 必须贯穿五步一致；本实现**每轮配对换一个新 `uniqueid`**——主机按它给挂起会话建索引
  （Sunshine `map_id_sess`），复用旧 id 会得到 409 且主机不会再次弹出 PIN。
- **客户端身份**：自签名 X.509，RSA-2048、`SHA256withRSA`、20 年、`CN=NVIDIA GameStream Client`；证书 PEM 用
  LF 换行（CRLF 会被主机拒收）。**上线的证书是 PEM 文本的 hex**（`clientcert` 参数、以及主机返回的
  `plaincert`），主机按 PEM 解析（Sunshine `crypto::x509` 走 `PEM_read_bio_X509`）——把 DER 的 hex 当作
  参数会配对失败。私钥落盘用 HUKS 信封加密（不可导出的 AES-256-GCM 密钥），证书明文落盘；
  不用 `certManager`/ASSET：前者私钥不可导出、无法用于 mTLS 握手，后者容量 ≤ 1KB 放不下 PKCS8 私钥。
- **配对原语**（hex 进 hex 出）：`aesKey = SHA256(salt || pin)[0:16]`——**恒 SHA-256**（Sunshine 与
  GFE 7+ 都如此；不要按版本回退 SHA-1）。AES-128-ECB、零填充、无 IV；哈希里串入的是**证书签名值字节**
  （ASN.1 BIT STRING 内容），不是公钥。
- **存储**：目录 `<dir>/moonlight/`（`dir` 为应用沙箱 files dir）；pin 按 **host** 存 `server_<key>.der`
  （客户端身份 `client.crt` / `client.key.enc` 全局一份），host key 归一化（小写、去尾点）后定位文件。
  主机身份（用于去重）不是地址，而是 serverinfo 的 **`uniqueid`**（即 Moonlight 的 `NvComputer::uuid`）——
  同一台机器写成 IP / 主机名会得到不同 pin，所以上层按 `uniqueid` 去重；主机应答了**不同的 `uniqueid`**
  （重装 / 换机）时旧 pin 判为失效并清除、重新配对；删除连接 / 改主机时也清掉该主机的 pin（见
  [`moonlight-connection.md`](moonlight-connection.md) §4）。

## 3. 控制端点与参数名

- **控制端点参数名（Sunshine）**：`/launch` 必填 `rikey` / `rikeyid` / `localAudioPlayMode` / `appid`，
  分辨率用 `mode=WxHxFPS`；HDR 是 `hdrMode`、环绕是 `surroundAudioInfo`（不是 `hdr`/`surroundaudio`），
  手柄掩码是 `gcmap`（本实现与 `remoteControllersBitmap` 一起填当前插着的手柄数，见
  [`moonlight-native.md`](moonlight-native.md) §2.5）。**没有 `appid=0` 的"桌面"**：未指定应用时先取 `/applist`
  选一个（优先名为 Desktop 的）。
  （`/appasset` 的 `AssetType`、`audioConfiguration` 等同理；改动前先核对标准客户端 Moonlight Android / Sunshine。）
- **客户端名称 `clientname`**：Sunshine 定义并实际消费的参数，`/pair`(getservercert) 与 `/launch`、`/resume`
  都读取（缺省 `Named Zako` / `unknown`），配对名进 Web UI 与 `root.pairname`；foundation-sunshine 还用
  `launch_session->client_name` 做客户端标识、按名查 VDD 物理尺寸，动态码率 API 亦按它路由（必填）。官方
  客户端只发硬编码 `devicename=roth`（Sunshine 不读）；本实现发 `deviceInfo.marketName`（空则 `productModel`，
  再空回退 `roth`）。
- **主机被占用**：`/launch` 会回 400 `An app is already running on this host`；客户端可 `GET /cancel`
  （HTTPS + mTLS）结束在跑的应用后再试，`/resume` 则复用既有会话。
- **明文 `serverinfo` 恒 `PairStatus=0`**：Sunshine 只在 HTTPS 请求里按 `uniqueid` 置 1，所以"是否已配对"
  以本地存的 pin 为准（有 pin 即跳过配对），不能只信明文 serverinfo。
- **剪贴板 blob 端点（基地版扩展）**：`POST /api/v1/clipboard/blob`（raw bytes + `X-Clipboard-Mime`，回 `{id,size,expires_in}`）
  与 `GET /api/v1/clipboard/blob/<id>`（raw bytes，`Content-Type` 回带 mime），走与 `applist`/`launch` 同一套
  HTTPS + mTLS + 证书 pin（端口取 serverinfo 的 `HttpsPort`，默认 47984）。`MoonlightHttp` 增加了二进制请求体支持，
  `MoonlightClient` 提供 `getClipboardBlob` / `postClipboardBlob`；用途见 [`moonlight-clipboard.md`](moonlight-clipboard.md) §5。
