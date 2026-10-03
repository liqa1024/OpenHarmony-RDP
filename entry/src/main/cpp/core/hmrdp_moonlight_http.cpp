/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) control surface - see hmrdp_moonlight_http.h.
 *
 * A single request is one self-contained HTTP/1.1 exchange (over TLS or plain
 * TCP): fresh context, fresh socket, no process-global state, so several control
 * requests can run concurrently without sharing anything.
 */
#include "hmrdp_moonlight_http.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hmrdp_log.h"

#ifdef HMRDP_HAVE_MOONLIGHT_HTTP

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hmrdp {
namespace {

constexpr size_t kMaxHeaderBytes = 64u * 1024u;
constexpr size_t kReadChunk = 16u * 1024u;

std::string LowerAscii(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
    begin++;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n')) {
    end--;
  }
  return text.substr(begin, end - begin);
}

// RFC 7230 chunked transfer decoding. Returns false only on a malformed stream;
// `complete` says whether the terminating zero-length chunk has been seen.
bool DecodeChunked(const std::string& in, size_t start, std::string& out, bool& complete) {
  size_t pos = start;
  out.clear();
  complete = false;
  while (true) {
    const size_t lineEnd = in.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      return true;
    }
    std::string sizeLine = Trim(in.substr(pos, lineEnd - pos));
    const size_t semi = sizeLine.find(';');
    if (semi != std::string::npos) {
      sizeLine = sizeLine.substr(0, semi);
    }
    errno = 0;
    const long size = std::strtol(sizeLine.c_str(), nullptr, 16);
    if (errno != 0 || size < 0) {
      return false;
    }
    const size_t dataStart = lineEnd + 2;
    if (size == 0) {
      complete = in.find("\r\n", dataStart) != std::string::npos;
      return true;
    }
    if (in.size() < dataStart + static_cast<size_t>(size) + 2u) {
      return true;
    }
    out.append(in, dataStart, static_cast<size_t>(size));
    pos = dataStart + static_cast<size_t>(size) + 2u;
  }
}

// Certificate pin: accept any peer when the pin is empty (first contact), else
// require the leaf certificate to be byte-identical to the stored DER. Replacing
// OpenSSL's default chain verification is intentional - the host is self-signed
// and trust is by pinned certificate, exactly like the official clients.
int PinnedVerify(X509_STORE_CTX* store, void* arg) {
  const std::string* pin = static_cast<const std::string*>(arg);
  if (pin == nullptr || pin->empty()) {
    return 1;
  }
  X509* leaf = X509_STORE_CTX_get0_cert(store);
  if (leaf == nullptr) {
    return 0;
  }
  unsigned char* der = nullptr;
  const int length = i2d_X509(leaf, &der);
  const bool match = length > 0 && static_cast<size_t>(length) == pin->size() &&
                     std::memcmp(der, pin->data(), static_cast<size_t>(length)) == 0;
  OPENSSL_free(der);
  return match ? 1 : 0;
}

std::string PeerCertDer(SSL* ssl) {
  X509* cert = SSL_get1_peer_certificate(ssl);
  if (cert == nullptr) {
    return std::string();
  }
  unsigned char* der = nullptr;
  const int length = i2d_X509(cert, &der);
  std::string out;
  if (length > 0) {
    out.assign(reinterpret_cast<const char*>(der), static_cast<size_t>(length));
  }
  OPENSSL_free(der);
  X509_free(cert);
  return out;
}

// Connects a TCP socket to host:port with a bounded connect timeout. Returns the
// fd, or -1 with `errcode` set.
int ConnectTcp(const std::string& host, int port, int timeoutMs, std::string& errcode) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  const std::string portText = std::to_string(port);
  const int gai = getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
  if (gai != 0 || result == nullptr) {
    errcode = "resolve";
    return -1;
  }

  int fd = -1;
  for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      rc = poll(&pfd, 1, timeoutMs);
      if (rc > 0) {
        int soError = 0;
        socklen_t len = sizeof(soError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len) == 0 && soError == 0) {
          rc = 0;
        } else {
          rc = -1;
          errno = soError;
        }
      } else if (rc == 0) {
        errno = ETIMEDOUT;
        rc = -1;
      }
    }
    if (rc == 0) {
      fcntl(fd, F_SETFL, flags);
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    errcode = "connect";
    return -1;
  }
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

// One connected transport: TLS (with a pin/optional client certificate) or plain
// TCP. `fd` is owned by the SSL object for TLS (BIO_CLOSE) and by this struct
// otherwise.
struct Transport {
  bool tls = false;
  int fd = -1;
  SSL* ssl = nullptr;
  SSL_CTX* ctx = nullptr;
};

void TransportClose(Transport& transport) {
  if (transport.tls) {
    SSL_free(transport.ssl);  // closes the socket (BIO_CLOSE)
    SSL_CTX_free(transport.ctx);
  } else if (transport.fd >= 0) {
    close(transport.fd);
  }
}

// Sets up the transport. TLS performs the handshake; plain does nothing. Returns
// a stable error code, or "" on success.
std::string TransportOpen(Transport& transport, const MoonlightHttpRequest& request, int fd) {
  transport.fd = fd;
  if (!request.useTls) {
    return std::string();
  }
  transport.ctx = SSL_CTX_new(TLS_client_method());
  if (transport.ctx == nullptr) {
    return "tls";
  }
  // The pin is read by the verify callback during the handshake, on this thread,
  // so the request outlives it.
  SSL_CTX_set_cert_verify_callback(transport.ctx, PinnedVerify,
                                   const_cast<std::string*>(&request.pinnedServerCertDer));
  SSL_CTX_set_verify(transport.ctx, SSL_VERIFY_PEER, nullptr);

  transport.ssl = SSL_new(transport.ctx);
  if (transport.ssl == nullptr) {
    return "tls";
  }
  BIO* bio = BIO_new_socket(fd, BIO_CLOSE);
  if (bio == nullptr) {
    return "tls";
  }
  SSL_set_bio(transport.ssl, bio, bio);
  SSL_set_tlsext_host_name(transport.ssl, request.host.c_str());

  if (!request.clientCertPem.empty()) {
    BIO* certBio = BIO_new_mem_buf(request.clientCertPem.data(),
                                   static_cast<int>(request.clientCertPem.size()));
    X509* cert = certBio != nullptr ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr)
                                    : nullptr;
    if (certBio != nullptr) {
      BIO_free(certBio);
    }
    BIO* keyBio = BIO_new_mem_buf(request.clientKeyPem.data(),
                                  static_cast<int>(request.clientKeyPem.size()));
    EVP_PKEY* key = keyBio != nullptr
                        ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr)
                        : nullptr;
    if (keyBio != nullptr) {
      BIO_free(keyBio);
    }
    if (cert == nullptr || key == nullptr || SSL_use_certificate(transport.ssl, cert) != 1 ||
        SSL_use_PrivateKey(transport.ssl, key) != 1) {
      HMRDP_LOGE("moonlight http: client certificate/key rejected");
      X509_free(cert);
      EVP_PKEY_free(key);
      return "tls";
    }
    X509_free(cert);
    EVP_PKEY_free(key);
  }

  if (SSL_connect(transport.ssl) != 1) {
    HMRDP_LOGW("moonlight http: tls handshake failed (%{public}s)",
               ERR_error_string(ERR_get_error(), nullptr));
    return "tls";
  }
  return std::string();
}

enum class ReadStatus { kOk, kEof, kTimeout, kError };

ReadStatus TransportRead(Transport& transport, char* buffer, int length, int* out) {
  if (transport.tls) {
    const int n = SSL_read(transport.ssl, buffer, length);
    if (n > 0) {
      *out = n;
      return ReadStatus::kOk;
    }
    const int sslError = SSL_get_error(transport.ssl, n);
    if (sslError == SSL_ERROR_ZERO_RETURN ||
        (sslError == SSL_ERROR_SYSCALL && errno == 0 && n == 0)) {
      return ReadStatus::kEof;
    }
    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE ||
        (sslError == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))) {
      return ReadStatus::kTimeout;
    }
    return ReadStatus::kError;
  }
  const ssize_t n = recv(transport.fd, buffer, static_cast<size_t>(length), 0);
  if (n > 0) {
    *out = static_cast<int>(n);
    return ReadStatus::kOk;
  }
  if (n == 0) {
    return ReadStatus::kEof;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return ReadStatus::kTimeout;
  }
  return ReadStatus::kError;
}

bool TransportWriteAll(Transport& transport, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    if (transport.tls) {
      const int n =
          SSL_write(transport.ssl, data.data() + sent, static_cast<int>(data.size() - sent));
      if (n <= 0) {
        return false;
      }
      sent += static_cast<size_t>(n);
    } else {
      const ssize_t n = send(transport.fd, data.data() + sent, data.size() - sent, 0);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<size_t>(n);
    }
  }
  return true;
}

MoonlightHttpResponse Fail(const std::string& code) {
  MoonlightHttpResponse response;
  response.ok = false;
  response.error = code;
  return response;
}

std::string BuildRequest(const MoonlightHttpRequest& request) {
  std::string out;
  out.reserve(256 + request.body.size());
  out += request.method;
  out += ' ';
  out += request.path;
  out += " HTTP/1.1\r\nHost: ";
  out += request.host;
  out += ':';
  out += std::to_string(request.port);
  out += "\r\nAccept: */*\r\nConnection: close\r\n";
  if (!request.body.empty()) {
    out += "Content-Type: ";
    out += request.contentType;
    out += "\r\nContent-Length: ";
    out += std::to_string(request.body.size());
    out += "\r\n";
  }
  out += "\r\n";
  out += request.body;
  return out;
}

MoonlightHttpResponse Perform(const MoonlightHttpRequest& request) {
  if (request.host.empty()) {
    return Fail("request");
  }

  std::string errcode;
  const int fd = ConnectTcp(request.host, request.port, request.timeoutMs, errcode);
  if (fd < 0) {
    HMRDP_LOGW("moonlight http: connect %{public}s:%{public}d failed", request.host.c_str(),
               request.port);
    return Fail(errcode);
  }

  Transport transport;
  const std::string openError = TransportOpen(transport, request, fd);
  if (!openError.empty()) {
    TransportClose(transport);
    return Fail(openError);
  }
  MoonlightHttpResponse response;
  if (transport.tls) {
    response.serverCertDer = PeerCertDer(transport.ssl);
  }

  if (!TransportWriteAll(transport, BuildRequest(request))) {
    HMRDP_LOGW("moonlight http: write failed");
    TransportClose(transport);
    return Fail("io");
  }

  // Read until the frame is complete (Content-Length / chunked) or the peer
  // closes.
  std::string raw;
  std::string decoded;
  size_t headerEnd = std::string::npos;
  long contentLength = -1;
  bool chunked = false;
  bool haveHeaders = false;
  bool bodyComplete = false;
  bool done = false;
  bool timedOut = false;
  while (!done) {
    char buffer[kReadChunk];
    int n = 0;
    const ReadStatus status = TransportRead(transport, buffer, static_cast<int>(sizeof(buffer)), &n);
    if (status == ReadStatus::kOk) {
      raw.append(buffer, static_cast<size_t>(n));
      if (!haveHeaders) {
        headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
          haveHeaders = true;
          size_t lineEnd = raw.find("\r\n");
          if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            lineEnd = headerEnd;
          }
          const std::string statusLine = raw.substr(0, lineEnd);
          const size_t firstSpace = statusLine.find(' ');
          const size_t secondSpace = firstSpace == std::string::npos
                                         ? std::string::npos
                                         : statusLine.find(' ', firstSpace + 1);
          if (firstSpace != std::string::npos) {
            const std::string code = statusLine.substr(
                firstSpace + 1,
                secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
            response.status = std::strtol(code.c_str(), nullptr, 10);
          }
          size_t pos = lineEnd + 2;
          while (pos < headerEnd) {
            const size_t next = raw.find("\r\n", pos);
            if (next == std::string::npos || next > headerEnd) {
              break;
            }
            const std::string line = raw.substr(pos, next - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
              const std::string name = LowerAscii(Trim(line.substr(0, colon)));
              const std::string value = Trim(line.substr(colon + 1));
              if (name == "content-length") {
                contentLength = std::strtol(value.c_str(), nullptr, 10);
              } else if (name == "transfer-encoding" &&
                         LowerAscii(value).find("chunked") != std::string::npos) {
                chunked = true;
              }
            }
            pos = next + 2;
          }
        }
      }
      if (haveHeaders) {
        const size_t bodyStart = headerEnd + 4;
        if (chunked) {
          bool complete = false;
          if (!DecodeChunked(raw, bodyStart, decoded, complete)) {
            TransportClose(transport);
            return Fail("http");
          }
          if (complete) {
            response.body = decoded;
            bodyComplete = true;
            done = true;
          }
        } else if (contentLength >= 0) {
          if (raw.size() - bodyStart >= static_cast<size_t>(contentLength)) {
            response.body = raw.substr(bodyStart, static_cast<size_t>(contentLength));
            bodyComplete = true;
            done = true;
          }
        }
        // No length and not chunked: keep reading until the peer closes.
      }
      if (!haveHeaders && raw.size() > kMaxHeaderBytes) {
        TransportClose(transport);
        return Fail("http");
      }
      continue;
    }
    if (status == ReadStatus::kEof) {
      // Peer closed. For a response with neither Content-Length nor chunked
      // framing the close is what delimits the body.
      if (haveHeaders && !chunked && contentLength < 0) {
        const size_t bodyStart = headerEnd + 4;
        response.body = raw.size() > bodyStart ? raw.substr(bodyStart) : std::string();
        bodyComplete = true;
      }
      done = true;
    } else if (status == ReadStatus::kTimeout) {
      timedOut = true;
      done = true;
    } else {
      HMRDP_LOGW("moonlight http: read failed");
      TransportClose(transport);
      return Fail("io");
    }
  }
  TransportClose(transport);

  if (!haveHeaders || !bodyComplete) {
    return Fail(timedOut ? "timeout" : "http");
  }
  response.ok = true;
  return response;
}

}  // namespace

std::string MoonlightHttpProbe() { return "1"; }

MoonlightHttpResponse MoonlightHttpPerform(const MoonlightHttpRequest& request) {
  return Perform(request);
}

}  // namespace hmrdp

#else  // !HMRDP_HAVE_MOONLIGHT_HTTP

namespace hmrdp {

std::string MoonlightHttpProbe() { return "0|no-moonlight-http"; }

MoonlightHttpResponse MoonlightHttpPerform(const MoonlightHttpRequest&) {
  MoonlightHttpResponse response;
  response.error = "no-http";
  return response;
}

}  // namespace hmrdp

#endif  // HMRDP_HAVE_MOONLIGHT_HTTP
