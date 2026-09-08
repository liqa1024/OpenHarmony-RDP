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
#include <string>

#include <freerdp/freerdp.h>

#include "hmrdp_renderer.h"

namespace hmrdp {

enum class SessionEvent {
  kConnected = 0,
  kDisconnected = 1,
  kError = 2,
  kFirstFrame = 3,
  kClipboardText = 4,
  kResize = 5,
  kCertificate = 6,
};

struct RdpOptions {
  std::string host;
  int port = 3389;
  std::string username;
  std::string password;
  std::string domain;
  int width = 1920;
  int height = 1080;
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

  explicit Session(int64_t id);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  int64_t id() const { return id_; }
  Renderer* renderer() { return &renderer_; }

  void SetEventFn(EventFn fn) { eventFn_ = std::move(fn); }

  bool Connect(const RdpOptions& options);
  void Disconnect();
  bool IsConnected() const { return connected_.load(); }

  bool SendMouse(uint16_t flags, uint16_t x, uint16_t y);
  bool SendExtendedMouse(uint16_t flags, uint16_t x, uint16_t y);
  bool SendKey(uint8_t scancode, bool down, bool extended);
  bool SendUnicode(uint16_t codepoint, bool down);
  bool SendSynchronize();

  void RequestResize(int width, int height);
  void SetClipboardEnabled(bool enabled);

  const std::string& LastError() const { return lastError_; }

  // Internal callbacks used by the FreeRDP glue.
  freerdp* instance() const { return instance_; }
  void HandlePostConnect();
  void HandleEndPaint();
  void HandleDesktopResize();
  void HandlePostDisconnect();
  void SetError(const std::string& error);

 private:
  void EventThread();
  void Emit(SessionEvent event, const std::string& data);

  int64_t id_;
  freerdp* instance_ = nullptr;
  Renderer renderer_;
  EventFn eventFn_;
  std::string lastError_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> clipboardEnabled_{true};
  void* thread_ = nullptr;
  bool firstFrameSent_ = false;
};

}  // namespace hmrdp

#endif  // HMRDP_SESSION_H
