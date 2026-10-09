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
#include "hmrdp_file_clipboard.h"
#include "hmrdp_frame_pipeline.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

enum class SessionPhase {
  kIdle = 0,
  kStarting = 1,
  kConnected = 2,
  kStopping = 3,
  kDestroyed = 4,
};

enum class SessionEvent {
  kConnected = 0,
  kDisconnected = 1,
  kError = 2,
  kFirstFrame = 3,
  kClipboardText = 4,
  kResize = 5,
  kCursorShape = 6,
  kCursorDefault = 7,
  kCursorHidden = 8,
  kClipboardHtml = 9,
  kClipboardImage = 10,
  kFileTransferProgress = 11,
  kFileTransferFailed = 12,
  kFileTransferDone = 13,
  kKeyIntercept = 14,
  kClipboardRtf = 15,
  kRemoteFileListRaw = 16,
  kRemoteFileChunk = 17,
  kRemoteClipboardChanged = 18,
  kAudioFormat = 19,
  kRemoteFormatList = 20,
  kRemoteFormatResponse = 21,
  kRemoteLockState = 22,
};

struct RdpOptions {
  std::string host;
  int port = 3389;
  std::string username;
  std::string password;
  std::string domain;
  int width = 1920;
  int height = 1080;
  int scalePercent = 0;
  bool ignoreCertificate = true;
  bool enableAudio = true;
  bool srEnabled = false;
  SuperResolutionBackend srBackend = SuperResolutionBackend::kXengine;
  int srRatioPercent = 0;
  std::string gatewayHost;
  int gatewayPort = 443;
  std::string gatewayUsername;
  std::string gatewayPassword;
  std::string gatewayDomain;
};

struct SessionTelemetry {
  uint64_t sampleSeq = 0;
  uint64_t windowElapsedMs = 0;
  uint64_t rxBytesWindow = 0;
  uint64_t txBytesWindow = 0;
  uint64_t framesWindow = 0;
  int32_t rttMs = -1;
  uint64_t windowFrames = 0;
  uint64_t zgxParseUs = 0;
  uint64_t decodeUs = 0;
  uint64_t composeUs = 0;
  uint64_t presentUs = 0;
  uint64_t presentWaitUs = 0;
  uint64_t syncUs = 0;
  uint64_t windowBytes = 0;
  uint64_t windowCommands = 0;
  int32_t audioRateHz = 0;
  uint64_t audioLostBytes = 0;
  uint64_t audioTotalBytes = 0;
  uint64_t gpuCopyUs = 0;
  uint64_t gpuSrUs = 0;
  uint64_t gpuBlitUs = 0;
};

class Session : public std::enable_shared_from_this<Session> {
 public:
  struct EventData {
    static constexpr size_t kMaxValues = 8;
    int64_t values[kMaxValues] = {0};
    std::string text;
  };

  using EventFn =
      std::function<void(SessionEvent, const EventData&, const std::vector<uint8_t>&)>;

  explicit Session();
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  FramePresenter* presenter() { return pipeline_.presenter(); }
  GfxWorkMeter* workMeter() { return pipeline_.meter(); }

  SessionTelemetry ReadTelemetry();

  void SetEventFn(EventFn fn) { eventFn_ = std::move(fn); }

  int OnAudioData(const void* data, size_t size, int sampleRate, int channels);

  int ReadAudio(uint8_t* dest, size_t size);

  void AudioFormat(int* sampleRate, int* channels);

  bool Connect(const RdpOptions& options);
  void Disconnect();

  // The FreeRDP context and cliprdr channel are freed only by Teardown() (RDP
  // thread); any other thread must hold a ContextLease while it touches either,
  // or it risks a use-after-free.
  struct ContextLease {
    freerdp* instance = nullptr;
    CliprdrClientContext* cliprdr = nullptr;
    bool valid() const { return instance != nullptr; }
  };
  ContextLease AcquireContext();
  void ReleaseContext();

  bool WaitDestroyed(uint32_t timeoutMs);

  bool SendMouse(uint16_t flags, uint16_t x, uint16_t y);
  bool SendTouch(uint32_t flags, int32_t finger, uint32_t pressure, int32_t x, int32_t y);
  bool SendKey(uint8_t scancode, bool down, bool extended);
  bool SendUnicode(uint16_t codepoint, bool down);
  bool SendLockState(uint32_t flags);

  bool ReassertLockState(uint32_t flags);

  bool SetSuppressed(bool suppressed);

  static void SetTouchHighRate(bool enabled);

  static void SetRdpCursor(bool enabled);

  static void SetHardwareAccel(bool enabled);

  static void SetGfxCapture(bool enabled, const std::string& dir);

  void SetLocalClipboardText(const std::string& utf8);
  void SetLocalClipboardHtmlRaw(const uint8_t* cfHtml, size_t byteCount,
                                const std::string& plainText);
  void SetLocalClipboardImage(const uint8_t* dib, size_t dibLen,
                              const uint8_t* png, size_t pngLen);

  void SetLocalFileOffer(const uint8_t* descriptor, size_t descriptorLen,
                         const std::vector<std::string>& paths);

  bool RequestRemoteFileContents(uint32_t listIndex, bool sizeOnly, uint64_t offset,
                                 uint32_t length);

  void CancelFileTransfer();

  bool RequestRemoteFormat(uint32_t formatId, uint32_t kind);

  void WithdrawLocalFileClipboard();

  void HandlePostConnect();
  void Teardown();
  void HandleFrameBegin();
  void HandleBeginPaint();
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void HandleCliprdrConnected(CliprdrClientContext* cliprdr);
  UINT HandleLocalFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST* request);
  UINT HandleRemoteFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE* response);
  void OnNetworkCharacteristics(uint32_t baseRtt, uint32_t averageRtt, uint32_t bandwidth);
  void SetGfxContext(void* gfx);
  void* gfxContext() const { return gfxContext_; }
  void StartGfxWorker(void* gfx);
  void StopGfxWorker();
  void HandleGfxData(const uint8_t* data, size_t size);

  void HandlePointerShape(uint32_t width, uint32_t height, uint32_t hotX, uint32_t hotY,
                          uint32_t xorBpp, const uint8_t* xorMask, uint32_t xorLen,
                          const uint8_t* andMask, uint32_t andLen);
  void HandlePointerDefault();
  void HandlePointerHidden();
  void HandleRemoteLockState(uint32_t ledFlags);

  UINT OnCliprdrMonitorReady();
  UINT OnCliprdrServerFormatList(const CLIPRDR_FORMAT_LIST* formatList);
  UINT OnCliprdrServerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST* request);
  UINT OnCliprdrServerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE* response);
  void SetError(const std::string& error);
  void SetError(uint32_t code, const std::string& error);

 private:
  void MarkDestroyed();
  void EventThread();
  void Emit(SessionEvent event);
  void Emit(SessionEvent event, const std::string& text);
  void EmitValues(SessionEvent event, std::initializer_list<int64_t> values);
  void EmitValues(SessionEvent event, std::initializer_list<int64_t> values,
                  const std::vector<uint8_t>& bytes);
  void EmitBytes(SessionEvent event, const std::vector<uint8_t>& bytes);
  void EmitError(SessionEvent event, uint32_t code, const std::string& message);
  void Emit(SessionEvent event, const EventData& data, const std::vector<uint8_t>& bytes);
  bool SendSynchronize(uint32_t flags);
  void AfterPresent();
  void SampleTelemetry();

  // Every dynamic channel shares the drdynvc dispatch thread, so the frame
  // pipeline must run on this session's own worker thread (GfxWorkerLoop), not
  // inline in HandleGfxData, or a heavy frame blocks audio and input.
  void GfxWorkerLoop();

  freerdp* instance_ = nullptr;
  FramePipeline pipeline_;
  AudioSink audio_;
  EventFn eventFn_;
  std::string lastError_;
  uint32_t lastErrorCode_ = 0;

  std::atomic<SessionPhase> phase_{SessionPhase::kIdle};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> clipboardReady_{false};
  std::mutex lifecycleMutex_;
  std::condition_variable leasesCv_;
  int activeLeases_ = 0;
  bool tearingDown_ = false;
  std::mutex destroyedMutex_;
  std::condition_variable destroyedCv_;
  bool destroyed_ = false;
  bool firstFrameSent_ = false;
  std::atomic<uint32_t> frameCount_{0};
  void* gfxContext_ = nullptr;
  std::thread gfxWorkerThread_;
  std::mutex gfxQueueMutex_;
  std::condition_variable gfxQueueCv_;
  std::deque<std::vector<uint8_t>> gfxQueue_;
  std::atomic<bool> gfxWorkerRun_{false};
  std::atomic<size_t> gfxQueueBytes_{0};
  std::atomic<bool> gfxQueueHighWaterWarned_{false};
  void* gfxWorkerContext_ = nullptr;
  // 超分: the server sizes the pointer bitmap in *session* pixels, so the
  // cursor bitmap is reported enlarged by this factor and ArkTS scales it by
  // the same amount the desktop is upscaled by (1.0 without 超分).
  double desktopMagnification_ = 1.0;
  std::atomic<uint32_t> netCharBaseRtt_{0};
  std::atomic<uint32_t> netCharAverageRtt_{0};
  std::atomic<uint32_t> netCharBandwidth_{0};
  mutable std::mutex telemetryMutex_;
  SessionTelemetry telemetry_;
  bool metricsStarted_ = false;
  uint64_t lastMetricsTick_ = 0;
  uint64_t lastInBytes_ = 0;
  uint64_t lastOutBytes_ = 0;
  uint64_t lastFrameCount_ = 0;
  uint64_t telemetrySeq_ = 0;
  std::atomic<uint32_t> localLocks_{0};
  std::atomic<bool> localLocksKnown_{false};

  enum class LocalClipKind { kNone = 0, kText = 1, kHtml = 2, kImage = 3, kRtf = 4,
                             kFiles = 5 };

  void AdvertiseLocalClipboard();

  CliprdrClientContext* cliprdrLocked() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return cliprdr_;
  }

  // Data responses carry no format id, so at most one request may be in
  // flight; anything newer is deferred.
  UINT SendRemoteDataRequest(UINT32 formatId, LocalClipKind kind);

  CliprdrClientContext* cliprdr_ = nullptr;
  std::mutex clipboardMutex_;
  std::string localClipboardUtf16_;
  bool localClipboardValid_ = false;
  std::string localClipboardHtml_;
  std::string localClipboardUtf16FromHtml_;
  std::string localClipboardDib_;
  std::string localClipboardPng_;
  std::vector<std::string> localClipboardFilePaths_;
  std::string localClipboardFileDescriptor_;
  LocalClipKind localClipKind_ = LocalClipKind::kNone;
  bool fileContentsPending_ = false;
  uint32_t pendingFileContentsIndex_ = 0;
  bool pendingFileContentsSizeOnly_ = false;
  uint64_t localTransferDone_ = 0;
  uint64_t localTransferTotal_ = 0;
  bool remoteRequestInFlight_ = false;
  LocalClipKind remoteRequestKind_ = LocalClipKind::kNone;
};

}  // namespace hmrdp

#endif  // HMRDP_SESSION_H
