/*
 * HmRdp - HarmonyOS RDP client
 * GameStream pairing crypto primitives - see hmrdp_moonlight_pair.h.
 *
 * The algorithm matches Moonlight's PairingManager (Android) / nvpairingmanager
 * (Qt): the salt+PIN derive an AES-128-ECB key, the handshake carries
 * hash-then-AES-ECB messages, and RSA signs/verifies the secrets. Certificate
 * signatures (the ASN.1 BIT STRING contents) are what the hashes embed.
 */
#include "hmrdp_moonlight_pair.h"

#include <cstdint>
#include <string>

#include "hmrdp_log.h"
#include "hmrdp_moonlight_cred.h"

#ifdef HMRDP_HAVE_MOONLIGHT_HTTP

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

namespace hmrdp {
namespace {

std::string HexEncode(const std::string& bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0x0F]);
  }
  return out;
}

int HexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool HexDecode(const std::string& hex, std::string* out) {
  if (hex.size() % 2 != 0) {
    return false;
  }
  out->clear();
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = HexNibble(hex[i]);
    const int lo = HexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out->push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

std::string RandomBytes(size_t length) {
  std::string out(length, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&out[0]), static_cast<int>(length)) != 1) {
    return std::string();
  }
  return out;
}

// SHA-256 (32 bytes) or SHA-1 (20 bytes), matching the server generation.
std::string Hash(bool sha256, const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  const EVP_MD* md = sha256 ? EVP_sha256() : EVP_sha1();
  if (EVP_Digest(data.data(), data.size(), digest, &length, md, nullptr) != 1) {
    return std::string();
  }
  return std::string(reinterpret_cast<char*>(digest), length);
}

// AES-128-ECB, no padding; the input is zero-padded to a whole block, exactly as
// the Android and Qt clients use the raw block cipher.
std::string AesEcb(bool encrypt, const std::string& key, const std::string& data) {
  if (key.size() != 16 || data.empty()) {
    return std::string();
  }
  const size_t padded = (data.size() + 15u) & ~size_t(15u);
  std::string input = data;
  input.resize(padded, '\0');
  std::string output(padded + 16u, '\0');

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    return std::string();
  }
  int outLen = 0;
  int finalLen = 0;
  bool ok = false;
  if (encrypt) {
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                            reinterpret_cast<const unsigned char*>(key.data()), nullptr) == 1 &&
         EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
         EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&output[0]), &outLen,
                           reinterpret_cast<const unsigned char*>(input.data()),
                           static_cast<int>(input.size())) == 1 &&
         EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&output[outLen]), &finalLen) == 1;
  } else {
    ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                            reinterpret_cast<const unsigned char*>(key.data()), nullptr) == 1 &&
         EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
         EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&output[0]), &outLen,
                           reinterpret_cast<const unsigned char*>(input.data()),
                           static_cast<int>(input.size())) == 1 &&
         EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&output[outLen]), &finalLen) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    return std::string();
  }
  output.resize(static_cast<size_t>(outLen + finalLen));
  return output;
}

// The server sends <plaincert> as hex(DER); accept PEM too, defensively.
X509* ParseCertificate(const std::string& data) {
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(data.data());
  X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(data.size()));
  if (cert != nullptr) {
    return cert;
  }
  BIO* bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
  if (bio == nullptr) {
    return nullptr;
  }
  cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return cert;
}

std::string CertSignatureBytes(const std::string& certData) {
  X509* cert = ParseCertificate(certData);
  if (cert == nullptr) {
    return std::string();
  }
  const ASN1_BIT_STRING* signature = nullptr;
  X509_get0_signature(&signature, nullptr, cert);
  std::string out;
  if (signature != nullptr) {
    out.assign(reinterpret_cast<const char*>(ASN1_STRING_get0_data(signature)),
               static_cast<size_t>(ASN1_STRING_length(signature)));
  }
  X509_free(cert);
  return out;
}

std::string SignSha256(EVP_PKEY* key, const std::string& data) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr || EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) != 1 ||
      EVP_DigestSignUpdate(ctx, data.data(), data.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::string();
  }
  size_t length = 0;
  if (EVP_DigestSignFinal(ctx, nullptr, &length) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::string();
  }
  std::string signature(length, '\0');
  if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(&signature[0]), &length) != 1) {
    EVP_MD_CTX_free(ctx);
    return std::string();
  }
  EVP_MD_CTX_free(ctx);
  signature.resize(length);
  return signature;
}

bool VerifySha256(const std::string& certData, const std::string& data,
                  const std::string& signature) {
  X509* cert = ParseCertificate(certData);
  if (cert == nullptr) {
    return false;
  }
  EVP_PKEY* pubKey = X509_get_pubkey(cert);
  X509_free(cert);
  if (pubKey == nullptr) {
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  const bool ok =
      ctx != nullptr && EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pubKey) == 1 &&
      EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1 &&
      EVP_DigestVerifyFinal(ctx, reinterpret_cast<const unsigned char*>(signature.data()),
                            signature.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pubKey);
  return ok;
}

// Reads the client private key of a stored credential out of its PKCS8 DER.
EVP_PKEY* ClientKey(const std::string& dir) {
  const MoonlightCredential credential = MoonlightLoadCredential(dir);
  if (!credential.ok) {
    return nullptr;
  }
  BIO* bio = BIO_new_mem_buf(credential.keyPkcs8Der.data(),
                             static_cast<int>(credential.keyPkcs8Der.size()));
  EVP_PKEY* key = bio != nullptr ? d2i_PKCS8PrivateKey_bio(bio, nullptr, nullptr, nullptr) : nullptr;
  BIO_free(bio);
  return key;
}

}  // namespace

std::string MoonlightPairProbe() { return "1"; }

std::string MoonlightPairDeriveKey(int serverMajorVersion, const std::string& saltHex,
                                   const std::string& pin) {
  std::string salt;
  if (!HexDecode(saltHex, &salt)) {
    return std::string();
  }
  std::string key = Hash(serverMajorVersion >= 7, salt + pin);
  if (key.size() < 16) {
    return std::string();
  }
  key.resize(16);
  return HexEncode(key);
}

std::string MoonlightAesEcb(bool encrypt, const std::string& keyHex, const std::string& dataHex) {
  std::string key;
  std::string data;
  if (!HexDecode(keyHex, &key) || !HexDecode(dataHex, &data)) {
    return std::string();
  }
  const std::string out = AesEcb(encrypt, key, data);
  return out.empty() ? std::string() : HexEncode(out);
}

std::string MoonlightPairChallengeResponse(int serverMajorVersion, const std::string& keyHex,
                                           const std::string& serverChallengeHex,
                                           const std::string& clientCertSignatureHex,
                                           const std::string& clientSecretHex) {
  std::string key;
  std::string challenge;
  std::string signature;
  std::string secret;
  if (!HexDecode(keyHex, &key) || !HexDecode(serverChallengeHex, &challenge) ||
      !HexDecode(clientCertSignatureHex, &signature) || !HexDecode(clientSecretHex, &secret)) {
    return std::string();
  }
  std::string digest = Hash(serverMajorVersion >= 7, challenge + signature + secret);
  if (digest.empty()) {
    return std::string();
  }
  digest.resize(32, '\0');
  const std::string encrypted = AesEcb(true, key, digest);
  return encrypted.empty() ? std::string() : HexEncode(encrypted);
}

std::string MoonlightPairExpectedResponse(int serverMajorVersion,
                                          const std::string& randomChallengeHex,
                                          const std::string& serverCertSignatureHex,
                                          const std::string& serverSecretHex) {
  std::string challenge;
  std::string signature;
  std::string secret;
  if (!HexDecode(randomChallengeHex, &challenge) || !HexDecode(serverCertSignatureHex, &signature) ||
      !HexDecode(serverSecretHex, &secret)) {
    return std::string();
  }
  const std::string digest = Hash(serverMajorVersion >= 7, challenge + signature + secret);
  return digest.empty() ? std::string() : HexEncode(digest);
}

std::string MoonlightCertSignature(const std::string& certHex) {
  std::string cert;
  if (!HexDecode(certHex, &cert)) {
    return std::string();
  }
  const std::string signature = CertSignatureBytes(cert);
  return signature.empty() ? std::string() : HexEncode(signature);
}

std::string MoonlightSignClientSecret(const std::string& dir, const std::string& clientSecretHex) {
  std::string secret;
  if (!HexDecode(clientSecretHex, &secret)) {
    return std::string();
  }
  EVP_PKEY* key = ClientKey(dir);
  if (key == nullptr) {
    return std::string();
  }
  const std::string signature = SignSha256(key, secret);
  EVP_PKEY_free(key);
  return signature.empty() ? std::string() : HexEncode(signature);
}

bool MoonlightVerifyServerSecret(const std::string& serverCertHex,
                                 const std::string& serverSecretHex,
                                 const std::string& serverSignatureHex) {
  std::string cert;
  std::string secret;
  std::string signature;
  if (!HexDecode(serverCertHex, &cert) || !HexDecode(serverSecretHex, &secret) ||
      !HexDecode(serverSignatureHex, &signature)) {
    return false;
  }
  return VerifySha256(cert, secret, signature);
}

std::string MoonlightRandomHex(int length) {
  if (length < 1 || length > 256) {
    return std::string();
  }
  const std::string bytes = RandomBytes(static_cast<size_t>(length));
  return bytes.empty() ? std::string() : HexEncode(bytes);
}

std::string MoonlightClientKeyPem(const std::string& dir) {
  const MoonlightCredential credential = MoonlightLoadCredential(dir);
  if (!credential.ok) {
    return std::string();
  }
  BIO* bio = BIO_new_mem_buf(credential.keyPkcs8Der.data(),
                             static_cast<int>(credential.keyPkcs8Der.size()));
  EVP_PKEY* key = bio != nullptr ? d2i_PKCS8PrivateKey_bio(bio, nullptr, nullptr, nullptr) : nullptr;
  BIO_free(bio);
  if (key == nullptr) {
    return std::string();
  }
  BIO* pem = BIO_new(BIO_s_mem());
  std::string out;
  if (pem != nullptr &&
      PEM_write_bio_PKCS8PrivateKey(pem, key, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(pem, &mem);
    if (mem != nullptr) {
      out.assign(mem->data, mem->length);
    }
  }
  BIO_free(pem);
  EVP_PKEY_free(key);
  return out;
}

}  // namespace hmrdp

#else  // !HMRDP_HAVE_MOONLIGHT_HTTP

namespace hmrdp {

std::string MoonlightPairProbe() { return "0|no-pair"; }
std::string MoonlightPairDeriveKey(int, const std::string&, const std::string&) {
  return std::string();
}
std::string MoonlightAesEcb(bool, const std::string&, const std::string&) { return std::string(); }
std::string MoonlightPairChallengeResponse(int, const std::string&, const std::string&,
                                           const std::string&, const std::string&) {
  return std::string();
}
std::string MoonlightPairExpectedResponse(int, const std::string&, const std::string&,
                                          const std::string&) {
  return std::string();
}
std::string MoonlightCertSignature(const std::string&) { return std::string(); }
std::string MoonlightSignClientSecret(const std::string&, const std::string&) {
  return std::string();
}
bool MoonlightVerifyServerSecret(const std::string&, const std::string&, const std::string&) {
  return false;
}
std::string MoonlightRandomHex(int) { return std::string(); }
std::string MoonlightClientKeyPem(const std::string&) { return std::string(); }

}  // namespace hmrdp

#endif  // HMRDP_HAVE_MOONLIGHT_HTTP
