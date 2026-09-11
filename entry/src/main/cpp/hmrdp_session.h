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
#include "hmrdp_renderer.h"

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

  Renderer* renderer() { return &renderer_; }

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
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void HandleCliprdrConnected(CliprdrClientContext* cliprdr);

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

  freerdp* instance_ = nullptr;
  Renderer renderer_;
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
