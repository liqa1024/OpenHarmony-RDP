/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session wrapper.
 */
#ifndef HMRDP_SESSION_H
#define HMRDP_SESSION_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>

#include "hmrdp_audio.h"
#include "hmrdp_gfx_work.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

// Explicit lifecycle of one connection attempt. A Session object is created per
// attempt and never reused, so the phases only ever move forward:
//   kIdle -> kStarting -> kConnected -> kStopping -> kDestroyed
// and a failed attempt goes kStarting -> kStopping -> kDestroyed (its terminal
// event is a single kError). Every transition is validated; an illegal one is
// logged and refused instead of silently corrupting the lifecycle.
enum class SessionPhase {
  kIdle = 0,
  kStarting = 1,
  kConnected = 2,
  kStopping = 3,
  kDestroyed = 4,
};

// The shared CPU (gdi) frame host (hmrdp_gfx_cpu.h): the zero-copy wait, its
// `sync` accounting and the present, implemented once for the live session and
// the offline replay. Held by pointer here to keep the header light.
class GdiFrameHost;

enum class SessionEvent {
  kConnected = 0,
  kDisconnected = 1,
  kError = 2,
  kFirstFrame = 3,
  kClipboardText = 4,
  kResize = 5,
  // Remote pointer updates. kCursorShape carries "<w>,<h>,<hotX>,<hotY>|<base64 BGRA>";
  // the others carry no payload and ask the UI to restore/hide the system cursor.
  kCursorShape = 6,
  kCursorDefault = 7,
  kCursorHidden = 8,
  // Rich local clipboard payloads pulled from the server on demand. Html
  // carries the HTML fragment; Image carries "<w>,<h>|<base64 BGRA>".
  kClipboardHtml = 9,
  kClipboardImage = 10,
  // Per-second session telemetry: "<rttMs>|<rxBps>|<txBps>|<fps>|<audioRateHz>|
  // <audioLossBp>|<zgxParseUs>|<decodeUs>|<composeUs>|<presentUs>|<bytesPerFrame>|
  // <dutyPermille>|<cmdsPerFrame>|<syncUs>|<presentWaitUs>|<gpuCopyUs>|<gpuSrUs>|
  // <gpuBlitUs>". rttMs is -1 while the server has not reported network
  // characteristics. There is no pre-summed client-work total: from index 6 on are
  // the per-frame phases and the blocked sub-items, and each reader adds the ones it
  // wants - the toolbar sums the four work phases; `syncUs` and `presentWaitUs` are
  // blocked and stay out, and the trailing three are GPU time (the upload, the 超分辨率
  // upscale and the letterbox), which is not CPU work either (see EmitMetrics).
  kMetrics = 11,
  // Remote clipboard holds files: "<count>|<totalBytes>". The toolbar's 复制
  // button downloads them into the sandbox on demand (PullRemoteFiles).
  kClipboardFileList = 12,
  // Transfer progress for both directions:
  // "<direction>|<doneBytes>|<totalBytes>" with direction 0 = remote -> local,
  // 1 = local -> remote.
  kFileTransferProgress = 13,
  // Download finished: the absolute sandbox paths, one per line, in list order.
  kClipboardFilesReady = 14,
  // A transfer stopped before completing; the payload is a human-readable reason.
  kFileTransferFailed = 15,
  // Upload finished: the server has pulled every advertised byte. Payload is
  // the number of files. (Downloads finish with kClipboardFilesReady instead.)
  kFileTransferDone = 16,
};

struct RdpOptions {
  std::string host;
  int port = 3389;
  std::string username;
  std::string password;
  std::string domain;
  int width = 1920;
  int height = 1080;
  // Remote desktop DPI scale factor as a percentage (100 = 100%). Zero leaves
  // the FreeRDP default untouched.
  int scalePercent = 0;
  int colorDepth = 32;
  bool ignoreCertificate = true;
  bool enableAudio = true;
  bool enableGfx = true;
  bool enableH264 = true;
  bool enableRemoteFx = true;
  // Frame-rate cap in frames per second (0 = uncapped). Frames above the cap are
  // never decoded: the cap holds each RDPGFX frame back before its decode, and
  // the frame acknowledge the server waits for is only written when that frame
  // ends (native/scripts/patch-steps/09-tcp-frameloop-qos.ps1).
  int maxFps = 0;
  // 超分: `width`/`height` are the *output* resolution. When this is on the
  // session asks the server for output ÷ srRatioPercent and the Vulkan presenter
  // upscales each frame back to the output resolution (no-op on the GLES
  // presenter). See SettingsStore.defaultScalePercent for the matching scale.
  bool srEnabled = false;
  // Which upscaler runs when `srEnabled` is on: the platform's XEngine, or the
  // presenter's own FSR (see hmrdp_presenter.h). Ignored when 超分 is off.
  SuperResolutionBackend srBackend = SuperResolutionBackend::kXengine;
  int srRatioPercent = 0;
  int performanceFlags = 0;
  std::string gatewayHost;
  int gatewayPort = 443;
  std::string gatewayUsername;
  std::string gatewayPassword;
  std::string gatewayDomain;
};

class Session : public std::enable_shared_from_this<Session> {
 public:
  using EventFn = std::function<void(SessionEvent, const std::string&)>;

  explicit Session();
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // The presenter the ArkTS surface lifecycle drives (setSurface/updateSurface/
  // clearSurface): Vulkan when it can present, GLES otherwise (hmrdp_presenter.h).
  FramePresenter* presenter() { return presenter_.get(); }

  void SetEventFn(EventFn fn) { eventFn_ = std::move(fn); }

  // Internal: forwards a PCM buffer produced by the rdpsnd backend to the
  // session's audio output.
  void OnAudioData(const void* data, size_t size, int sampleRate, int channels);

  bool Connect(const RdpOptions& options);
  void Disconnect();

  // The FreeRDP context and the cliprdr channel outlive neither each other nor
  // the RDP thread that owns them. Any other thread (UI input, clipboard, the
  // gdi callbacks) must take a lease for the whole time it touches either:
  //   ContextLease lease = AcquireContext();
  //   if (!lease.valid()) return;
  //   ... use lease.instance / lease.cliprdr ...
  //   ReleaseContext();
  // Teardown() (the RDP thread) stops handing out leases, waits for the
  // outstanding ones to be returned and only then frees, so a lease can never
  // point at freed memory. The wait is bounded: a stuck lease holder delays the
  // free but never turns it into a use-after-free.
  struct ContextLease {
    freerdp* instance = nullptr;
    CliprdrClientContext* cliprdr = nullptr;
    bool valid() const { return instance != nullptr; }
  };
  ContextLease AcquireContext();
  void ReleaseContext();

  // Blocks until the RDP thread has freed the context and marked the session
  // kDestroyed (or the timeout elapses). Returns true when it is really gone.
  // A timeout is not a failure of the object's lifetime - the RDP thread still
  // owns and will eventually free it - it only means the caller stopped waiting.
  bool WaitDestroyed(uint32_t timeoutMs);

  bool SendMouse(uint16_t flags, uint16_t x, uint16_t y);
  bool SendTouch(uint32_t flags, int32_t finger, uint32_t pressure, int32_t x, int32_t y);
  bool SendKey(uint8_t scancode, bool down, bool extended);
  bool SendUnicode(uint16_t codepoint, bool down);

  // Process-global RDPEI frame pacing. When enabled, touch contacts are sent at
  // the full input rate instead of the upstream ~20ms (50Hz) coalescing. No-op
  // unless FreeRDP was built with the rdpei interval patch
  // (native/scripts/patch-freerdp.ps1).
  static void SetTouchHighRate(bool enabled);

  // Process-global toggle for remote cursor handling. When disabled, RDP
  // pointer updates are ignored entirely so the HarmonyOS default cursor is
  // kept (the pre-cursor-support behaviour). Applied when a session connects.
  static void SetRdpCursor(bool enabled);

  // Process-global "硬件加速" preference: present frames through Vulkan (on) or
  // stay on the GLES presenter with no Vulkan in the session (off). Applies from
  // the next presenter on, so the pre-connect call is what a session sees
  // (hmrdp_presenter.h). Decoding itself is always FreeRDP's gdi path.
  static void SetHardwareAccel(bool enabled);

  // Dev-only capture of the incoming RemoteFX/Progressive GFX surface streams,
  // written to `dir` as hmrdp_rfx.bin (see doc_agent/gfx-engine.md §6).
  static void SetRfxDump(bool enabled, const std::string& dir);

  // Local clipboard text (UTF-8) pushed from ArkTS; advertised to the server as
  // CF_UNICODETEXT. Safe to call from the UI thread.
  void SetLocalClipboardText(const std::string& utf8);
  // Local HTML clipboard (UTF-8 fragment) pushed from ArkTS; advertised as
  // CF_HTML with a plain-text fallback so text-only targets still paste.
  void SetLocalClipboardHtml(const std::string& html);
  // Local image clipboard pushed from ArkTS. `pixelFormat` is the HarmonyOS
  // image.PixelMapFormat of `pixels`; the buffer is converted to a CF_DIB
  // payload for the server. Safe to call from the UI thread.
  void SetLocalClipboardImage(uint32_t width, uint32_t height, int32_t pixelFormat,
                              const uint8_t* pixels, size_t byteCount);

  // Local files (absolute sandbox paths) pushed from ArkTS; advertised to the
  // server as a FileGroupDescriptorW + FileContents clipboard. Safe to call from
  // the UI thread.
  void SetLocalClipboardFiles(const std::vector<std::string>& paths);

  // Downloads the file list the server last advertised into `destDir` (which
  // must already exist). Safe to call from the UI thread; progress arrives as
  // kFileTransferProgress, completion as kClipboardFilesReady.
  void PullRemoteFiles(const std::string& destDir);

  // Aborts an in-flight transfer (either direction); partial downloads are
  // removed and the advertised file list stops being served. Safe to call from
  // the UI thread.
  void CancelFileTransfer();

  // Drops the advertised file clipboard by advertising an empty text clipboard,
  // so the server discards the file formats and a later paste on the remote side
  // finds nothing instead of pulling the same files a second time. Uploads call
  // this once every advertised byte has been served (see HandleLocalFileContents-
  // Request); it is the withdrawal an upload cancelled before any byte was pulled
  // performs. No-op unless the local clipboard is currently a file list.
  void WithdrawLocalFileClipboard();

  // Internal callbacks used by the FreeRDP glue. The FreeRDP instance itself is
  // only reachable through AcquireContext() (see above), never as a raw accessor.
  void HandlePostConnect();
  // Runs on the RDP thread when its connect/loop has ended: frees the FreeRDP
  // context and the resources only this session used. The UI thread never frees
  // them - it only signals through Disconnect() - so nothing is freed while the
  // RDP thread may still be using it.
  void Teardown();
  // Both wait for the GPU to release the desktop buffer this frame writes into
  // (which may be the presenter's own buffer, see AttachPresenterDesktopBuffer);
  // the wait is idempotent per frame, so whichever runs first is the one that
  // waits:
  //   * HandleFrameBegin at the GFX START_FRAME (before this frame's writes),
  //   * HandleBeginPaint at gdi's BeginPaint (before the compose).
  void HandleFrameBegin();
  // Runs once per frame at the GFX START_FRAME on the GFX worker thread, before
  // the frame is decoded: the frame-rate cap (`maxFps`) sleeps here until this
  // frame's slot opens. Because the frame acknowledge is written when this frame
  // ends, holding the frame here is what the server sees as the client's frame
  // rate - it then sends fewer frames. Running it on the worker (not the shared
  // drdynvc dispatch thread) is what keeps the wait from holding audio/input back.
  void HandleStartFrame();
  void HandleBeginPaint();
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void HandleCliprdrConnected(CliprdrClientContext* cliprdr);
  // Called by the cliprdr glue: serve the peer's request for one of the files
  // we advertised, and receive one chunk of a file we requested.
  UINT HandleLocalFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST* request);
  UINT HandleRemoteFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE* response);
  // Pushed by the autodetect callback with the server-reported network
  // characteristics (0 = not reported yet). FreeRDP's client does not store
  // these itself, so the values are captured here.
  void OnNetworkCharacteristics(uint32_t baseRtt, uint32_t averageRtt, uint32_t bandwidth);
  // The GFX channel context whose decode callback libhmrdp wrapped, so it can be
  // unregistered on disconnect. Stored as void* to keep the header light.
  void SetGfxContext(void* gfx);
  void* gfxContext() const { return gfxContext_; }
  // GFX data offload lifecycle (see GfxWorkSetDataSinkHook): started when the
  // live GFX channel connects and stopped before the context is freed. The sink
  // calls HandleGfxData on the drdynvc thread.
  void StartGfxWorker(void* gfx);
  void StopGfxWorker();
  void HandleGfxData(const uint8_t* data, size_t size);

  // Remote pointer (cursor) updates. The server sends the cursor as a bitmap
  // plus hot spot; the UI converts it into a HarmonyOS system cursor. Returning
  // to the default system cursor / hiding it is signalled separately.
  void HandlePointerShape(uint32_t width, uint32_t height, uint32_t hotX, uint32_t hotY,
                          uint32_t xorBpp, const uint8_t* xorMask, uint32_t xorLen,
                          const uint8_t* andMask, uint32_t andLen);
  void HandlePointerDefault();
  void HandlePointerHidden();

  // Clipboard channel callbacks (invoked on the RDP thread).
  UINT OnCliprdrMonitorReady();
  UINT OnCliprdrServerFormatList(const CLIPRDR_FORMAT_LIST* formatList);
  UINT OnCliprdrServerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST* request);
  UINT OnCliprdrServerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE* response);
  // `code` is a FreeRDP error code (FREERDP_ERROR_*) or 0 for internal errors.
  void SetError(const std::string& error);
  void SetError(uint32_t code, const std::string& error);

 private:
  // Marks the terminal phase and wakes WaitDestroyed(). Called by Teardown() (RDP
  // thread) and by Connect()'s pre-thread failure paths, which have no RDP thread
  // to tear anything down.
  void MarkDestroyed();
  void EventThread();
  void Emit(SessionEvent event, const std::string& data);
  // Frame telemetry shared by every present path. The present's own meter phases
  // are fed by the shared frame host (hmrdp_gfx_cpu.h), so this only does the
  // per-frame bookkeeping.
  void AfterPresent();
  // Samples RTT / frame rate / transport throughput and emits kMetrics.
  void EmitMetrics();

  // GFX data offload (see GfxWorkSetDataSinkHook). Every dynamic channel shares
  // the drdynvc dispatch thread, so running the frame pipeline there (ZGX
  // decompress, decode, composite, present) holds audio and input back behind a
  // heavy frame. HandleGfxData runs on that thread and only copies the chunk;
  // GfxWorkerLoop processes it here, on this session's own thread, through the
  // same replay entry the offline CPU route uses (HmrdpGfxReplayRecv).
  void GfxWorkerLoop();

  freerdp* instance_ = nullptr;
  // CPU frame presenter (Vulkan by default, GLES fallback). The live frame
  // pipeline all runs on the session's GFX worker thread (architecture.md §4),
  // and the backend still serialises presents itself so the offline replay route
  // can drive its own presenter from the replay thread.
  std::unique_ptr<FramePresenter> presenter_;
  // Attach/zero-copy wait/present for this session's gdi (the same host the
  // offline replay uses), see hmrdp_gfx_cpu.h.
  std::unique_ptr<GdiFrameHost> frameHost_;
  AudioOutput audio_;
  EventFn eventFn_;
  std::string lastError_;
  uint32_t lastErrorCode_ = 0;

  // Forward-only lifecycle of this attempt (SessionPhase). Connect() is the only
  // kIdle -> kStarting transition; Teardown() is the only one into kDestroyed.
  std::atomic<SessionPhase> phase_{SessionPhase::kIdle};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> clipboardReady_{false};
  // Context lifetime. lifecycleMutex_ guards instance_, cliprdr_, activeLeases_
  // and tearingDown_; leasesCv_ is waited on (under that mutex) by Teardown()
  // until the outstanding leases are returned, so the context (and the channel,
  // which dies with it) is never freed under a user. Disconnect() (UI thread)
  // only signals; Teardown() (RDP thread) is the sole freer.
  std::mutex lifecycleMutex_;
  std::condition_variable leasesCv_;
  int activeLeases_ = 0;
  bool tearingDown_ = false;
  // Publishes the fully-freed state to WaitDestroyed() without reusing
  // lifecycleMutex_ (that one may be held by a lease holder during teardown).
  std::mutex destroyedMutex_;
  std::condition_variable destroyedCv_;
  bool destroyed_ = false;
  bool firstFrameSent_ = false;
  // Frames drawn since the last metrics sample (incremented in HandleEndPaint,
  // drained on the event thread).
  std::atomic<uint32_t> frameCount_{0};
  // Per-frame client work for the live telemetry: the capture hook and the
  // wrapped GFX callbacks feed it while this session is connected, and the event
  // thread drains one window per second (see hmrdp_gfx_work.h for what each
  // phase covers - the same meter, fed through the same hooks, is used by the
  // offline replay so the two figures are comparable).
  GfxWorkMeter meter_;
  void* gfxContext_ = nullptr;
  // GFX offload worker and its queue (see HandleGfxData). gfxWorkerContext_ is
  // captured at start and used only while a lease is held, so teardown cannot
  // free it under a send.
  std::thread gfxWorkerThread_;
  std::mutex gfxQueueMutex_;
  std::condition_variable gfxQueueCv_;
  std::deque<std::vector<uint8_t>> gfxQueue_;
  std::atomic<bool> gfxWorkerRun_{false};
  std::atomic<size_t> gfxQueueBytes_{0};
  void* gfxWorkerContext_ = nullptr;
  // Frame-rate cap read from the connect options (0 = uncapped), and the release
  // stamp of the last frame it let through. Both are touched only on the GFX
  // thread (the START_FRAME hook), except for the connect-time write.
  int maxFps_ = 0;
  uint64_t lastFramePaceUs_ = 0;
  // 超分辨率 magnification of the remote desktop (output ÷ session resolution).
  // The server sizes the pointer bitmap in *session* pixels, so the cursor is
  // magnified by the same factor - the CPU resample in UpscaleBgra - to stay in
  // step with the upscaled picture; 1.0 without it. Written at connect, read on
  // the RDP thread.
  double desktopMagnification_ = 1.0;
  // Recent audio (lost,total) byte counts, one slot per metric window, summed to
  // give a short-term glitch rate instead of a cumulative counter.
  uint64_t audioLostWindow_[5] = {0};
  uint64_t audioTotalWindow_[5] = {0};
  uint32_t audioWindowIndex_ = 0;
  // Server-reported network characteristics from the autodetect channel.
  std::atomic<uint32_t> netCharBaseRtt_{0};
  std::atomic<uint32_t> netCharAverageRtt_{0};
  std::atomic<uint32_t> netCharBandwidth_{0};
  // Telemetry baseline. metricsStarted_ is cleared on connect so a reconnect
  // re-establishes the throughput/frame baseline.
  bool metricsStarted_ = false;
  uint64_t lastMetricsTick_ = 0;
  uint64_t lastInBytes_ = 0;
  uint64_t lastOutBytes_ = 0;
  uint32_t lastFrameCount_ = 0;
  // Hash of the last cursor bitmap sent to the UI; identical repeats (pointer
  // cache hits) are dropped so the system cursor is only re-installed on an
  // actual shape change.
  uint32_t cursorHash_ = 0;
  bool cursorHashValid_ = false;

  // Clipboard redirection state. The local clipboard is kept in the exact wire
  // form of each format it can provide. Only one kind is owned at a time, so the
  // server only ever requests formats we can answer.
  enum class LocalClipKind { kNone = 0, kText = 1, kHtml = 2, kImage = 3, kRtf = 4,
                             kFiles = 5 };

  // A file advertised on the clipboard (either side). `size` is 0 when the
  // sender did not include FD_FILESIZE; the size is then fetched on demand.
  // Directories are advertised too (listIndex must stay aligned) but are
  // skipped: the file clipboard cannot stream them.
  struct ClipboardFileInfo {
    std::string name;
    uint64_t size = 0;
    bool sizeKnown = false;
    bool isDirectory = false;
  };

  // Pushes the local FormatList for the current kind (no-op until the channel
  // is ready). Called after the clipboard state changes.
  void AdvertiseLocalClipboard();

  // Synchronized snapshot of the cliprdr channel. A caller that is not the RDP
  // thread must hold a ContextLease for as long as it uses the returned channel:
  // the lease is what stops teardown from freeing it mid-use.
  CliprdrClientContext* cliprdrLocked() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return cliprdr_;
  }

  // Issues a clipboard data request to the server and marks it in flight. Data
  // responses carry no format id, so at most one request may be outstanding;
  // anything newer is deferred (see OnCliprdrServerFormatList).
  UINT SendRemoteDataRequest(UINT32 formatId, LocalClipKind kind);

  // Remote-file download state machine (see PullRemoteFiles). One request is in
  // flight at a time; StartNextRemoteFileStep drives the sequence and is called
  // once from PullRemoteFiles and then from every file-contents response.
  UINT StartNextRemoteFileStep();
  void FinishRemoteFileTransfer();
  void FailRemoteFileTransfer(const std::string& reason);
  // Aborts a transfer and removes its partial files; caller holds clipboardMutex_.
  void ResetRemoteFileDownloadLocked();

  CliprdrClientContext* cliprdr_ = nullptr;
  std::mutex clipboardMutex_;
  // NUL-terminated UTF-16LE (CF_UNICODETEXT wire form).
  std::string localClipboardUtf16_;
  bool localClipboardValid_ = false;
  // CF_HTML wire payload, plus its UTF-16LE plain-text fallback.
  std::string localClipboardHtml_;
  std::string localClipboardUtf16FromHtml_;
  // CF_DIB wire payload.
  std::string localClipboardDib_;
  // Local file list (absolute sandbox paths) advertised for the file clipboard.
  std::vector<std::string> localClipboardFilePaths_;
  LocalClipKind localClipKind_ = LocalClipKind::kNone;
  // Remote file download state. `remoteFiles_` is the descriptor list the
  // server last advertised; the rest is the transfer in progress.
  std::vector<ClipboardFileInfo> remoteFiles_;
  bool remoteFilesActive_ = false;
  std::string remoteFilesDestDir_;
  std::vector<std::string> remoteFilesPaths_;
  size_t remoteFileIndex_ = 0;
  uint64_t remoteFileOffset_ = 0;
  uint64_t remoteFileSize_ = 0;
  bool remoteFileSizeKnown_ = false;
  bool remoteFileSizePending_ = false;
  FILE* remoteFileHandle_ = nullptr;
  uint64_t remoteTransferDone_ = 0;
  uint64_t remoteTransferTotal_ = 0;
  // Local -> remote progress (bytes the server has pulled from us).
  uint64_t localTransferDone_ = 0;
  uint64_t localTransferTotal_ = 0;
  // Format the server used for "HTML Format" in its latest FormatList, kept for
  // bookkeeping; requests use the id that list carried.
  UINT32 remoteHtmlFormatId_ = 0;
  // Remote fetch serialisation. A data response carries no format id, so only
  // one request may be in flight; a FormatList arriving meanwhile is remembered
  // and fetched after the current response.
  bool remoteRequestInFlight_ = false;
  LocalClipKind remoteRequestKind_ = LocalClipKind::kNone;
  bool hasPendingRemoteRefresh_ = false;
  LocalClipKind pendingRefreshKind_ = LocalClipKind::kNone;
  UINT32 pendingRefreshFormatId_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_SESSION_H
