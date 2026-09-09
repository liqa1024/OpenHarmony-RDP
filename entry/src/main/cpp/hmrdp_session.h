/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session wrapper.
 */
#ifndef HMRDP_SESSION_H
#define HMRDP_SESSION_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>

#include "hmrdp_renderer.h"

namespace hmrdp {

enum class SessionEvent {
  kConnected = 0,
  kDisconnected = 1,
  kError = 2,
  kFirstFrame = 3,
  kClipboardText = 4,
  kResize = 5,
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

  bool Connect(const RdpOptions& options);
  void Disconnect();

  bool SendMouse(uint16_t flags, uint16_t x, uint16_t y);
  bool SendTouch(uint32_t flags, int32_t finger, uint32_t pressure, int32_t x, int32_t y);
  bool SendKey(uint8_t scancode, bool down, bool extended);
  bool SendUnicode(uint16_t codepoint, bool down);

  // Local clipboard text (UTF-8) pushed from ArkTS; advertised to the server as
  // CF_UNICODETEXT. Safe to call from the UI thread.
  void SetLocalClipboardText(const std::string& utf8);

  // Internal callbacks used by the FreeRDP glue.
  freerdp* instance() const { return instance_; }
  void HandlePostConnect();
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void HandleCliprdrConnected(CliprdrClientContext* cliprdr);

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
  EventFn eventFn_;
  std::string lastError_;
  uint32_t lastErrorCode_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> clipboardEnabled_{true};
  std::atomic<bool> clipboardReady_{false};
  void* thread_ = nullptr;
  bool firstFrameSent_ = false;

  // Clipboard redirection state. `localClipboardUtf16_` holds the current local
  // clipboard text as NUL-terminated UTF-16LE (the CF_UNICODETEXT wire form).
  CliprdrClientContext* cliprdr_ = nullptr;
  std::mutex clipboardMutex_;
  std::string localClipboardUtf16_;
  bool localClipboardValid_ = false;
};

}  // namespace hmrdp

#endif  // HMRDP_SESSION_H
