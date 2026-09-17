/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session wrapper.
 */
#ifndef HMRDP_SESSION_H
#define HMRDP_SESSION_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>

#include "hmrdp_audio.h"
#include "hmrdp_gfx_work.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

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
  // Per-second session telemetry: "<rttMs>|<rxBps>|<txBps>|<fps>|<localUs>|
  // <responseUs>|<audioRateHz>|<audioLossBp>|<zgxParseUs>|<decodeUs>|<composeUs>|
  // <presentUs>|<bytesPerFrame>|<dutyPermille>". rttMs is -1 while the server has
  // not reported network characteristics; the fields from index 8 on are the
  // per-frame breakdown of 本机 (see EmitMetrics).
  kMetrics = 11,
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
  bool enableClipboard = true;
  bool enableAudio = true;
  bool enableGfx = true;
  bool enableH264 = true;
  bool enableRemoteFx = true;
  int performanceFlags = 0;
  std::string gatewayHost;
  int gatewayPort = 443;
  std::string gatewayUsername;
  std::string gatewayPassword;
  std::string gatewayDomain;
};

class Session {
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

  // Process-global preference for hardware (GPU) RemoteFX decoding instead of
  // the CPU decoder. Read when a session connects (see doc_agent/gfx-engine.md §1).
  static void SetHardwareDecode(bool enabled);

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

  // Internal callbacks used by the FreeRDP glue.
  freerdp* instance() const { return instance_; }
  void HandlePostConnect();
  // Called from gdi's BeginPaint: before this frame's pixels are written into the
  // primary buffer, which may be the presenter's own buffer (see
  // AttachPresenterDesktopBuffer).
  void HandleBeginPaint();
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void HandleCliprdrConnected(CliprdrClientContext* cliprdr);
  // Pushed by the autodetect callback with the server-reported network
  // characteristics (0 = not reported yet). FreeRDP's client does not store
  // these itself, so the values are captured here.
  void OnNetworkCharacteristics(uint32_t baseRtt, uint32_t averageRtt, uint32_t bandwidth);
  // The GFX channel context whose decode callback libhmrdp wrapped, so it can be
  // unregistered on disconnect. Stored as void* to keep the header light.
  void SetGfxContext(void* gfx);
  void* gfxContext() const { return gfxContext_; }

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
  void EventThread();
  void Emit(SessionEvent event, const std::string& data);
  // Frame telemetry shared by every present path.
  void AfterPresent(uint64_t renderStartUs);
  // Samples RTT / frame rate / transport throughput and emits kMetrics.
  void EmitMetrics();
  // Records an input timestamp for the input-to-frame response measurement.
  // Called from the UI thread; only arms when the scene has been idle so the
  // sample is not polluted by continuous frame cadence.
  void MarkInput();

  freerdp* instance_ = nullptr;
  // CPU frame presenter (Vulkan by default, GLES fallback). FreeRDP feeds GFX
  // commands on one thread while gdi's EndPaint callback can fire on another, so
  // every present is serialised by the backend itself.
  std::unique_ptr<FramePresenter> presenter_;
  // True once gdi composes into the presenter's own desktop buffer (zero-copy
  // present, doc_agent/cpu-path.md §4). Retried every frame until it succeeds,
  // which is how the session picks it up when the surface arrives after connect.
  bool desktopAttached_ = false;
  AudioOutput audio_;
  EventFn eventFn_;
  std::string lastError_;
  uint32_t lastErrorCode_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> clipboardEnabled_{true};
  std::atomic<bool> clipboardReady_{false};
  void* thread_ = nullptr;
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
  // Ring of the most recent input-to-frame measurements; the emitted value is
  // their mean (this is a statistic, not a hard real-time figure).
  uint64_t responseSamplesUs_[5] = {0};
  uint32_t responseSampleCount_ = 0;
  uint32_t responseSampleIndex_ = 0;
  // Recent audio (lost,total) byte counts, one slot per metric window, summed to
  // give a short-term glitch rate instead of a cumulative counter.
  uint64_t audioLostWindow_[5] = {0};
  uint64_t audioTotalWindow_[5] = {0};
  uint32_t audioWindowIndex_ = 0;
  // Written from the RDP thread, read from the UI thread (MarkInput).
  std::atomic<uint64_t> lastFrameTickUs_{0};
  // UI thread -> RDP thread hand-off; 0 means "no input pending".
  std::atomic<uint64_t> pendingInputUs_{0};
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
  enum class LocalClipKind { kNone = 0, kText = 1, kHtml = 2, kImage = 3, kRtf = 4 };

  // Pushes the local FormatList for the current kind (no-op until the channel
  // is ready). Called after the clipboard state changes.
  void AdvertiseLocalClipboard();

  // Issues a clipboard data request to the server and marks it in flight. Data
  // responses carry no format id, so at most one request may be outstanding;
  // anything newer is deferred (see OnCliprdrServerFormatList).
  UINT SendRemoteDataRequest(UINT32 formatId, LocalClipKind kind);

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
  LocalClipKind localClipKind_ = LocalClipKind::kNone;
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
