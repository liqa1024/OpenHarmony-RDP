/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight client identity and trust storage - see hmrdp_moonlight_cred.h.
 *
 * Layout under <dir>/moonlight/:
 *   client.crt      self-signed client certificate, PEM (public)
 *   client.key.enc  PKCS8 private key, AES-256-GCM under a HUKS key:
 *                   "HMRDPML1" (8) | nonce (12) | ciphertext + tag (16)
 *   server_<key>.der  pinned server certificate per host, DER (public)
 *
 * The remaining bytes of a partially written file are never trusted: the magic
 * and the GCM tag both have to check out, otherwise the credential is treated as
 * corrupt and regenerated (which also clears the pins, forcing a re-pair).
 */
#include "hmrdp_moonlight_cred.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "hmrdp_log.h"

#ifdef HMRDP_HAVE_MOONLIGHT_HTTP

#include <huks/native_huks_api.h>
#include <huks/native_huks_param.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace hmrdp {
namespace {

constexpr const char* kSubDir = "moonlight";
constexpr const char* kCertFile = "client.crt";
constexpr const char* kKeyFile = "client.key.enc";
constexpr const char* kPinPrefix = "server_";
constexpr const char* kPinSuffix = ".der";

// HUKS wrapping key. Non-exportable, so the AES key never leaves the secure
// environment; only this alias crosses back into the app.
constexpr const char* kHuksAlias = "hmrdp_moonlight_cred_v1";
constexpr uint32_t kNonceSize = 12;
constexpr uint32_t kTagSize = 16;
constexpr char kMagic[8] = {'H', 'M', 'R', 'D', 'P', 'M', 'L', '1'};
constexpr char kAad[] = "HmRdpMoonlightCredV1";

std::mutex g_storeMutex;

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty() || a.back() == '/') {
    return a + b;
  }
  return a + "/" + b;
}

bool EnsureDir(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  return mkdir(path.c_str(), 0700) == 0;
}

bool ReadFile(const std::string& path, std::string* out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, read);
  }
  fclose(file);
  return true;
}

bool WriteFile(const std::string& path, const std::string& data) {
  // Write to a temporary file first: a torn write must not look like a valid
  // credential.
  const std::string tmp = path + ".tmp";
  FILE* file = fopen(tmp.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = data.empty() || fwrite(data.data(), 1, data.size(), file) == data.size();
  const bool flushed = fflush(file) == 0;
  fclose(file);
  if (!written || !flushed) {
    remove(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string HostFileName(const std::string& hostKey) {
  std::string name = hostKey;
  for (char& c : name) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!safe) {
      c = '_';
    }
  }
  if (name.empty()) {
    name = "default";
  }
  return std::string(kPinPrefix) + name + kPinSuffix;
}

// --- HUKS AES-256-GCM envelope ----------------------------------------------

OH_Huks_Result InitParamSet(OH_Huks_ParamSet** paramSet, const OH_Huks_Param* params,
                            uint32_t count) {
  OH_Huks_Result ret = OH_Huks_InitParamSet(paramSet);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    return ret;
  }
  ret = OH_Huks_AddParams(*paramSet, params, count);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    OH_Huks_FreeParamSet(paramSet);
    return ret;
  }
  ret = OH_Huks_BuildParamSet(paramSet);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    OH_Huks_FreeParamSet(paramSet);
  }
  return ret;
}

bool HuksKeyExists(const OH_Huks_Blob& alias) {
  const OH_Huks_Result result = OH_Huks_IsKeyItemExist(&alias, nullptr);
  return result.errorCode == OH_HUKS_SUCCESS;
}

bool EnsureHuksKey(const OH_Huks_Blob& alias, std::string* errorCode) {
  if (HuksKeyExists(alias)) {
    return true;
  }
  OH_Huks_Param params[] = {
      {.tag = OH_HUKS_TAG_ALGORITHM, .uint32Param = OH_HUKS_ALG_AES},
      {.tag = OH_HUKS_TAG_PURPOSE,
       .uint32Param = OH_HUKS_KEY_PURPOSE_ENCRYPT | OH_HUKS_KEY_PURPOSE_DECRYPT},
      {.tag = OH_HUKS_TAG_KEY_SIZE, .uint32Param = OH_HUKS_AES_KEY_SIZE_256},
      {.tag = OH_HUKS_TAG_PADDING, .uint32Param = OH_HUKS_PADDING_NONE},
      {.tag = OH_HUKS_TAG_BLOCK_MODE, .uint32Param = OH_HUKS_MODE_GCM},
  };
  OH_Huks_ParamSet* paramSet = nullptr;
  OH_Huks_Result ret = InitParamSet(&paramSet, params, sizeof(params) / sizeof(params[0]));
  if (ret.errorCode == OH_HUKS_SUCCESS) {
    ret = OH_Huks_GenerateKeyItem(&alias, paramSet, nullptr);
  }
  OH_Huks_FreeParamSet(&paramSet);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    HMRDP_LOGE("moonlight cred: HUKS key generation failed (%{public}d)", ret.errorCode);
    *errorCode = "huks";
    return false;
  }
  return true;
}

// Encrypts with the HUKS AES-256-GCM key; the output carries the GCM tag the
// same way OH_Huks returns it (tag appended).
bool HuksEncrypt(const OH_Huks_Blob& alias, const uint8_t* nonce, const uint8_t* plain,
                 size_t plainSize, std::string* out) {
  OH_Huks_Param params[] = {
      {.tag = OH_HUKS_TAG_ALGORITHM, .uint32Param = OH_HUKS_ALG_AES},
      {.tag = OH_HUKS_TAG_PURPOSE, .uint32Param = OH_HUKS_KEY_PURPOSE_ENCRYPT},
      {.tag = OH_HUKS_TAG_KEY_SIZE, .uint32Param = OH_HUKS_AES_KEY_SIZE_256},
      {.tag = OH_HUKS_TAG_PADDING, .uint32Param = OH_HUKS_PADDING_NONE},
      {.tag = OH_HUKS_TAG_BLOCK_MODE, .uint32Param = OH_HUKS_MODE_GCM},
      {.tag = OH_HUKS_TAG_NONCE, .blob = {.size = kNonceSize, .data = const_cast<uint8_t*>(nonce)}},
      {.tag = OH_HUKS_TAG_ASSOCIATED_DATA,
       .blob = {.size = static_cast<uint32_t>(sizeof(kAad) - 1),
                .data = reinterpret_cast<uint8_t*>(const_cast<char*>(kAad))}},
  };
  OH_Huks_ParamSet* paramSet = nullptr;
  OH_Huks_Result ret = InitParamSet(&paramSet, params, sizeof(params) / sizeof(params[0]));
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    return false;
  }
  uint8_t handleBuf[sizeof(uint64_t)] = {0};
  OH_Huks_Blob handle = {sizeof(uint64_t), handleBuf};
  std::vector<uint8_t> buffer(plainSize + 64);
  OH_Huks_Blob inData = {static_cast<uint32_t>(plainSize), const_cast<uint8_t*>(plain)};
  OH_Huks_Blob outData = {static_cast<uint32_t>(buffer.size()), buffer.data()};
  ret = OH_Huks_InitSession(&alias, paramSet, &handle, nullptr);
  if (ret.errorCode == OH_HUKS_SUCCESS) {
    ret = OH_Huks_FinishSession(&handle, paramSet, &inData, &outData);
  }
  OH_Huks_FreeParamSet(&paramSet);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    HMRDP_LOGE("moonlight cred: HUKS encrypt failed (%{public}d)", ret.errorCode);
    return false;
  }
  out->assign(reinterpret_cast<char*>(outData.data), outData.size);
  return true;
}

// Decrypts a HUKS GCM blob whose last 16 bytes are the tag (see HuksEncrypt).
bool HuksDecrypt(const OH_Huks_Blob& alias, const uint8_t* nonce, const uint8_t* cipher,
                 size_t cipherSize, std::string* out) {
  if (cipherSize <= kTagSize) {
    return false;
  }
  const size_t bodySize = cipherSize - kTagSize;
  OH_Huks_Param params[] = {
      {.tag = OH_HUKS_TAG_ALGORITHM, .uint32Param = OH_HUKS_ALG_AES},
      {.tag = OH_HUKS_TAG_PURPOSE, .uint32Param = OH_HUKS_KEY_PURPOSE_DECRYPT},
      {.tag = OH_HUKS_TAG_KEY_SIZE, .uint32Param = OH_HUKS_AES_KEY_SIZE_256},
      {.tag = OH_HUKS_TAG_PADDING, .uint32Param = OH_HUKS_PADDING_NONE},
      {.tag = OH_HUKS_TAG_BLOCK_MODE, .uint32Param = OH_HUKS_MODE_GCM},
      {.tag = OH_HUKS_TAG_NONCE, .blob = {.size = kNonceSize, .data = const_cast<uint8_t*>(nonce)}},
      {.tag = OH_HUKS_TAG_ASSOCIATED_DATA,
       .blob = {.size = static_cast<uint32_t>(sizeof(kAad) - 1),
                .data = reinterpret_cast<uint8_t*>(const_cast<char*>(kAad))}},
      {.tag = OH_HUKS_TAG_AE_TAG,
       .blob = {.size = kTagSize, .data = const_cast<uint8_t*>(cipher + bodySize)}},
  };
  OH_Huks_ParamSet* paramSet = nullptr;
  OH_Huks_Result ret = InitParamSet(&paramSet, params, sizeof(params) / sizeof(params[0]));
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    return false;
  }
  uint8_t handleBuf[sizeof(uint64_t)] = {0};
  OH_Huks_Blob handle = {sizeof(uint64_t), handleBuf};
  std::vector<uint8_t> buffer(bodySize + 64);
  OH_Huks_Blob inData = {static_cast<uint32_t>(bodySize), const_cast<uint8_t*>(cipher)};
  OH_Huks_Blob outData = {static_cast<uint32_t>(buffer.size()), buffer.data()};
  ret = OH_Huks_InitSession(&alias, paramSet, &handle, nullptr);
  if (ret.errorCode == OH_HUKS_SUCCESS) {
    ret = OH_Huks_FinishSession(&handle, paramSet, &inData, &outData);
  }
  OH_Huks_FreeParamSet(&paramSet);
  if (ret.errorCode != OH_HUKS_SUCCESS) {
    HMRDP_LOGW("moonlight cred: HUKS decrypt failed (%{public}d)", ret.errorCode);
    return false;
  }
  out->assign(reinterpret_cast<char*>(outData.data), outData.size);
  return true;
}

// --- credential encode/decode -----------------------------------------------

bool DecodeWrapped(const std::string& file, std::string* nonce, std::string* cipher) {
  const size_t header = sizeof(kMagic) + kNonceSize;
  if (file.size() <= header + kTagSize ||
      std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  nonce->assign(file.data() + sizeof(kMagic), kNonceSize);
  cipher->assign(file.data() + header, file.size() - header);
  return true;
}

bool WrapKey(const OH_Huks_Blob& alias, const std::string& plain, std::string* wrapped) {
  uint8_t nonce[kNonceSize];
  if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
    return false;
  }
  std::string cipher;
  if (!HuksEncrypt(alias, nonce, reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                   &cipher)) {
    return false;
  }
  std::string out(kMagic, sizeof(kMagic));
  out.append(reinterpret_cast<char*>(nonce), sizeof(nonce));
  out += cipher;
  *wrapped = std::move(out);
  return true;
}

bool UnwrapKey(const OH_Huks_Blob& alias, const std::string& wrapped, std::string* plain) {
  std::string nonce;
  std::string cipher;
  if (!DecodeWrapped(wrapped, &nonce, &cipher)) {
    return false;
  }
  return HuksDecrypt(alias, reinterpret_cast<const uint8_t*>(nonce.data()),
                     reinterpret_cast<const uint8_t*>(cipher.data()), cipher.size(), plain);
}

std::string NormalizePem(const std::string& pem) {
  std::string out;
  out.reserve(pem.size());
  for (char c : pem) {
    if (c != '\r') {
      out.push_back(c);
    }
  }
  return out;
}

std::string RootDir(const std::string& dir) { return Join(dir, kSubDir); }

// Loads the stored credential. Sets error "missing" when nothing is stored.
MoonlightCredential LoadLocked(const std::string& root) {
  MoonlightCredential cred;
  std::string certPem;
  std::string wrapped;
  if (!ReadFile(Join(root, kCertFile), &certPem) || !ReadFile(Join(root, kKeyFile), &wrapped)) {
    cred.error = "missing";
    return cred;
  }
  const std::string tmp = std::string(kHuksAlias);
  OH_Huks_Blob alias = {static_cast<uint32_t>(tmp.size()),
                        reinterpret_cast<uint8_t*>(const_cast<char*>(tmp.data()))};
  if (!HuksKeyExists(alias)) {
    // The wrapping key is gone (fresh install, cleared keystore): the stored
    // key cannot be recovered.
    cred.error = "corrupt";
    return cred;
  }
  std::string key;
  if (!UnwrapKey(alias, wrapped, &key)) {
    cred.error = "corrupt";
    return cred;
  }
  cred.ok = true;
  cred.certPem = certPem;
  cred.keyPkcs8Der = std::move(key);
  return cred;
}

bool StoreLocked(const std::string& root, const MoonlightClientIdentity& identity,
                 std::string* errorCode) {
  const std::string tmp = std::string(kHuksAlias);
  OH_Huks_Blob alias = {static_cast<uint32_t>(tmp.size()),
                        reinterpret_cast<uint8_t*>(const_cast<char*>(tmp.data()))};
  if (!EnsureHuksKey(alias, errorCode)) {
    return false;
  }
  std::string wrapped;
  if (!WrapKey(alias, identity.keyPkcs8Der, &wrapped)) {
    *errorCode = "huks";
    return false;
  }
  if (!WriteFile(Join(root, kCertFile), identity.certPem) ||
      !WriteFile(Join(root, kKeyFile), wrapped)) {
    *errorCode = "write";
    return false;
  }
  return true;
}

void ClearPinsLocked(const std::string& root) {
  DIR* dir = opendir(root.c_str());
  if (dir == nullptr) {
    return;
  }
  struct dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string name = entry->d_name;
    if (name.rfind(kPinPrefix, 0) == 0 && name.size() > strlen(kPinSuffix) &&
        name.compare(name.size() - strlen(kPinSuffix), strlen(kPinSuffix), kPinSuffix) == 0) {
      remove(Join(root, name).c_str());
    }
  }
  closedir(dir);
}

}  // namespace

std::string MoonlightCredentialProbe() { return "1"; }

bool MoonlightGenerateClientIdentity(MoonlightClientIdentity* out) {
  if (out == nullptr) {
    return false;
  }
  EVP_PKEY* pkey = nullptr;
  EVP_PKEY_CTX* keyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  bool ok = keyCtx != nullptr && EVP_PKEY_keygen_init(keyCtx) == 1 &&
            EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx, 2048) == 1 &&
            EVP_PKEY_keygen(keyCtx, &pkey) == 1;
  EVP_PKEY_CTX_free(keyCtx);
  if (!ok || pkey == nullptr) {
    HMRDP_LOGE("moonlight cred: RSA key generation failed");
    return false;
  }

  X509* cert = X509_new();
  unsigned char serialBytes[8];
  BIGNUM* serial = nullptr;
  ASN1_INTEGER* serialInt = nullptr;
  if (cert == nullptr || RAND_bytes(serialBytes, sizeof(serialBytes)) != 1) {
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return false;
  }
  serial = BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr);
  serialInt = BN_to_ASN1_INTEGER(serial, nullptr);
  BN_free(serial);
  if (serialInt == nullptr) {
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return false;
  }
  X509_set_serialNumber(cert, serialInt);
  ASN1_INTEGER_free(serialInt);

  X509_set_version(cert, 2);  // X.509 v3
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  // 20 years; matches the official clients' identity lifetime.
  X509_gmtime_adj(X509_getm_notAfter(cert), 20L * 365L * 24L * 3600L);
  X509_set_pubkey(cert, pkey);
  X509_NAME* name = X509_get_subject_name(cert);
  if (name == nullptr ||
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>("NVIDIA GameStream Client"),
                                 -1, -1, 0) != 1 ||
      X509_set_issuer_name(cert, name) != 1 || X509_sign(cert, pkey, EVP_sha256()) == 0) {
    HMRDP_LOGE("moonlight cred: self-signed certificate build failed");
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return false;
  }

  std::string certPem;
  BIO* certBio = BIO_new(BIO_s_mem());
  if (certBio == nullptr || PEM_write_bio_X509(certBio, cert) != 1) {
    BIO_free(certBio);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return false;
  }
  {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(certBio, &mem);
    if (mem != nullptr) {
      certPem.assign(mem->data, mem->length);
    }
  }
  BIO_free(certBio);

  std::string keyDer;
  BIO* keyBio = BIO_new(BIO_s_mem());
  if (keyBio == nullptr ||
      i2d_PKCS8PrivateKey_bio(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    BIO_free(keyBio);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return false;
  }
  {
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(keyBio, &mem);
    if (mem != nullptr) {
      keyDer.assign(mem->data, mem->length);
    }
  }
  BIO_free(keyBio);

  X509_free(cert);
  EVP_PKEY_free(pkey);

  if (certPem.empty() || keyDer.empty()) {
    return false;
  }
  out->certPem = NormalizePem(certPem);
  out->keyPkcs8Der = std::move(keyDer);
  return true;
}

MoonlightCredential MoonlightEnsureCredential(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_storeMutex);
  if (!EnsureDir(dir)) {
    MoonlightCredential cred;
    cred.error = "dir";
    return cred;
  }
  const std::string root = RootDir(dir);
  if (!EnsureDir(root)) {
    MoonlightCredential cred;
    cred.error = "dir";
    return cred;
  }

  MoonlightCredential cred = LoadLocked(root);
  if (cred.ok) {
    return cred;
  }
  if (cred.error == "corrupt") {
    // A new identity needs a fresh wrapping key and a fresh pairing.
    const std::string tmp = std::string(kHuksAlias);
    OH_Huks_Blob alias = {static_cast<uint32_t>(tmp.size()),
                          reinterpret_cast<uint8_t*>(const_cast<char*>(tmp.data()))};
    (void)OH_Huks_DeleteKeyItem(&alias, nullptr);
    remove(Join(root, kKeyFile).c_str());
    ClearPinsLocked(root);
    HMRDP_LOGW("moonlight cred: stored key was undecryptable, regenerating identity");
  }

  MoonlightClientIdentity identity;
  if (!MoonlightGenerateClientIdentity(&identity)) {
    MoonlightCredential failed;
    failed.error = "crypto";
    return failed;
  }
  std::string storeError;
  if (!StoreLocked(root, identity, &storeError)) {
    MoonlightCredential failed;
    failed.error = storeError;
    return failed;
  }
  // Read it back through the same decrypt path, so a wrap/unwrap mismatch is
  // caught here rather than at the first request.
  return LoadLocked(root);
}

MoonlightCredential MoonlightLoadCredential(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_storeMutex);
  return LoadLocked(RootDir(dir));
}

bool MoonlightClearCredential(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_storeMutex);
  const std::string root = RootDir(dir);
  const std::string tmp = std::string(kHuksAlias);
  OH_Huks_Blob alias = {static_cast<uint32_t>(tmp.size()),
                        reinterpret_cast<uint8_t*>(const_cast<char*>(tmp.data()))};
  (void)OH_Huks_DeleteKeyItem(&alias, nullptr);
  remove(Join(root, kCertFile).c_str());
  remove(Join(root, kKeyFile).c_str());
  ClearPinsLocked(root);
  HMRDP_LOGI("moonlight cred: cleared client identity and pins");
  return true;
}

std::string MoonlightLoadPinnedServerCert(const std::string& dir, const std::string& hostKey) {
  std::lock_guard<std::mutex> lock(g_storeMutex);
  std::string der;
  if (!ReadFile(Join(RootDir(dir), HostFileName(hostKey)), &der)) {
    return std::string();
  }
  return der;
}

bool MoonlightStorePinnedServerCert(const std::string& dir, const std::string& hostKey,
                                    const std::string& der) {
  if (der.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_storeMutex);
  const std::string root = RootDir(dir);
  if (!EnsureDir(dir) || !EnsureDir(root)) {
    return false;
  }
  return WriteFile(Join(root, HostFileName(hostKey)), der);
}

bool MoonlightClearPinnedServerCert(const std::string& dir, const std::string& hostKey) {
  std::lock_guard<std::mutex> lock(g_storeMutex);
  return remove(Join(RootDir(dir), HostFileName(hostKey)).c_str()) == 0;
}

bool MoonlightServerPublicKey(const std::string& certDer, std::string* modulus,
                              std::string* exponent) {
  if (certDer.empty() || modulus == nullptr || exponent == nullptr) {
    return false;
  }
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(certDer.data());
  X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(certDer.size()));
  if (cert == nullptr) {
    return false;
  }
  EVP_PKEY* pkey = X509_get_pubkey(cert);
  X509_free(cert);
  if (pkey == nullptr || EVP_PKEY_is_a(pkey, "RSA") != 1) {
    EVP_PKEY_free(pkey);
    return false;
  }
  BIGNUM* n = nullptr;
  BIGNUM* e = nullptr;
  const bool ok = EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n) == 1 &&
                  EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e) == 1 && n != nullptr &&
                  e != nullptr;
  EVP_PKEY_free(pkey);
  if (!ok) {
    BN_free(n);
    BN_free(e);
    return false;
  }
  modulus->resize(static_cast<size_t>(BN_num_bytes(n)));
  BN_bn2bin(n, reinterpret_cast<unsigned char*>(&(*modulus)[0]));
  exponent->resize(static_cast<size_t>(BN_num_bytes(e)));
  BN_bn2bin(e, reinterpret_cast<unsigned char*>(&(*exponent)[0]));
  BN_free(n);
  BN_free(e);
  return true;
}

}  // namespace hmrdp

#else  // !HMRDP_HAVE_MOONLIGHT_HTTP

namespace hmrdp {

std::string MoonlightCredentialProbe() { return "0|no-cred"; }

bool MoonlightGenerateClientIdentity(MoonlightClientIdentity*) { return false; }

MoonlightCredential MoonlightEnsureCredential(const std::string&) {
  MoonlightCredential cred;
  cred.error = "no-cred";
  return cred;
}

MoonlightCredential MoonlightLoadCredential(const std::string&) {
  MoonlightCredential cred;
  cred.error = "no-cred";
  return cred;
}

bool MoonlightClearCredential(const std::string&) { return false; }

std::string MoonlightLoadPinnedServerCert(const std::string&, const std::string&) {
  return std::string();
}

bool MoonlightStorePinnedServerCert(const std::string&, const std::string&, const std::string&) {
  return false;
}

bool MoonlightClearPinnedServerCert(const std::string&, const std::string&) { return false; }

bool MoonlightServerPublicKey(const std::string&, std::string*, std::string*) { return false; }

}  // namespace hmrdp

#endif  // HMRDP_HAVE_MOONLIGHT_HTTP
