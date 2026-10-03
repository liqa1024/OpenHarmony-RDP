/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight client identity and trust storage.
 *
 * Holds the self-signed GameStream client certificate/private key and the
 * per-host pinned server certificates. The public certificate and the pinned
 * server certificates are stored in the clear (they are public); the client
 * private key is stored AES-256-GCM encrypted under a non-exportable HUKS key,
 * decrypted into memory only when a request needs it. See
 * doc_agent/moonlight-native.md.
 */
#ifndef HMRDP_MOONLIGHT_CRED_H
#define HMRDP_MOONLIGHT_CRED_H

#include <string>

namespace hmrdp {

// A freshly generated client identity. `certPem` uses LF line endings (the host
// rejects CRLF); `keyPkcs8Der` is the unencrypted PKCS8 private key.
struct MoonlightClientIdentity {
  std::string certPem;
  std::string keyPkcs8Der;
};

// The stored client credential. The private key is decrypted into memory for the
// caller's use and never written in the clear.
struct MoonlightCredential {
  bool ok = false;
  std::string certPem;
  std::string keyPkcs8Der;
  // Stable code when !ok (the UI owns the wording):
  //   "no-cred"      this build has no Moonlight credential layer
  //   "dir"          the storage directory could not be created
  //   "huks"         HUKS key generation / envelope crypto failed
  //   "corrupt"      the stored key file could not be decrypted (HUKS key gone)
  //   "write"        a credential file could not be written
  //   "crypto"       the self-signed identity could not be generated
  std::string error;
};

// "1" / "0|<code>" capability probe, mirroring the other hmrdp probes.
std::string MoonlightCredentialProbe();

// Generates a self-signed GameStream client identity: RSA-2048, SHA256, 20-year
// validity, random serial, CN=NVIDIA GameStream Client (see Moonlight's Android
// and Qt clients). Returns false on failure.
bool MoonlightGenerateClientIdentity(MoonlightClientIdentity* out);

// Ensures the store under <dir>/moonlight has a client credential, creating one
// (and its HUKS wrapping key) on first use, and returns it.
MoonlightCredential MoonlightEnsureCredential(const std::string& dir);

// Loads the credential without generating one.
MoonlightCredential MoonlightLoadCredential(const std::string& dir);

// Deletes the client credential, its HUKS wrapping key and every pinned server
// certificate, forcing a fresh identity and a new pairing.
bool MoonlightClearCredential(const std::string& dir);

// Pinned server certificate (DER) for one host, or empty when none is stored.
// `hostKey` is the connection's host (optionally host:port).
std::string MoonlightLoadPinnedServerCert(const std::string& dir, const std::string& hostKey);
bool MoonlightStorePinnedServerCert(const std::string& dir, const std::string& hostKey,
                                    const std::string& der);
bool MoonlightClearPinnedServerCert(const std::string& dir, const std::string& hostKey);

// RSA modulus and exponent (big-endian), read from a DER certificate, for the
// GameStream pairing handshake. False when the certificate has no RSA key.
bool MoonlightServerPublicKey(const std::string& certDer, std::string* modulus,
                              std::string* exponent);

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_CRED_H
