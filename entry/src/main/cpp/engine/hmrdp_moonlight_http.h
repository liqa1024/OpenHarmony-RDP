/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) HTTPS control surface: one TLS + HTTP/1.1 request.
 *
 * The whole GameStream control protocol - /serverinfo, /pair, /applist,
 * /launch, /resume - is built out of this primitive. The native side owns only
 * TLS and HTTP framing; the response body is returned raw and parsed by the
 * caller (ArkTS, via @ohos.xml; see doc_agent/moonlight-native.md).
 *
 * This is deliberately independent of FreeRDP's freerdp_http_request, which is
 * hard-wired to port 443, the system CA store and form-urlencoded bodies: the
 * game-stream control port is configurable (47989/47984), the host is
 * self-signed, and paired requests need a client certificate (mutual TLS).
 */
#ifndef HMRDP_MOONLIGHT_HTTP_H
#define HMRDP_MOONLIGHT_HTTP_H

#include <string>

namespace hmrdp {

struct MoonlightHttpRequest {
  std::string method = "GET";
  std::string host;  // IP or host name, no scheme, no port
  int port = 47989;
  std::string path = "/";  // path + query, e.g. "/serverinfo?uuid=0"
  // Request body. `contentType` is only sent when the body is non-empty.
  std::string body;
  std::string contentType = "application/xml";
  // Plain HTTP when false, TLS when true. The GameStream control port is HTTP
  // (47989) for serverinfo / pair / unpair and HTTPS (47984) for the paired
  // endpoints; plain requests use neither pin nor client certificate.
  bool useTls = true;
  // TLS trust. When `pinnedServerCertDer` is non-empty the peer's leaf
  // certificate must equal it exactly (the paired state); when it is empty any
  // certificate is accepted, which is the first-contact pairing handshake every
  // official client starts with. The certificate the peer presented is always
  // reported back in the response so the caller can pin it after pairing.
  std::string pinnedServerCertDer;
  // Mutual TLS: attached when the connection is paired. PEM encoded.
  std::string clientCertPem;
  std::string clientKeyPem;
  int timeoutMs = 10000;
};

struct MoonlightHttpResponse {
  // True when the whole transport handshake and HTTP framing succeeded. A 4xx /
  // 5xx response is still ok (the caller decides what the status means).
  bool ok = false;
  // HTTP status code, 0 when !ok.
  long status = 0;
  // Raw response body (may be binary).
  std::string body;
  // Leaf certificate (DER) the TLS peer presented, for pinning after pairing.
  std::string serverCertDer;
  // Stable error code when !ok; the UI layer owns the Chinese wording:
  //   "no-http"   this build has no Moonlight HTTP layer (no OpenSSL headers)
  //   "request"   empty host / malformed request
  //   "resolve"   host name lookup failed
  //   "connect"   TCP connect failed or timed out
  //   "tls"       TLS handshake or certificate verification failed
  //   "io"        socket read/write failed
  //   "timeout"   the request exceeded timeoutMs
  //   "http"      the response could not be framed/parsed
  std::string error;
};

// "1" when the HTTP layer is built, else "0|no-moonlight-http". Mirrors the
// capability-probe pattern in hmrdp_vk_context (native returns a stable code).
std::string MoonlightHttpProbe();

// Performs one request synchronously. Blocking; callers must run it off the UI
// thread (a Moonlight host worker thread). No process-global state.
MoonlightHttpResponse MoonlightHttpPerform(const MoonlightHttpRequest& request);

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_HTTP_H
