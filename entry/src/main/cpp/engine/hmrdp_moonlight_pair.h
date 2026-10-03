/*
 * HmRdp - HarmonyOS RDP client
 * GameStream pairing crypto primitives.
 *
 * Pairing orchestration (and every XML field) lives in ArkTS, the same way the
 * official clients keep the protocol in one layer with a real XML parser. These
 * are the native operations it calls; all byte strings cross the N-API boundary
 * as lowercase hex.
 */
#ifndef HMRDP_MOONLIGHT_PAIR_H
#define HMRDP_MOONLIGHT_PAIR_H

#include <string>

namespace hmrdp {

// "1" / "0|<code>" capability probe.
std::string MoonlightPairProbe();

// AES key = hash(salt || pin_utf8)[0:16]; SHA-256 when `serverMajorVersion` >= 7,
// SHA-1 otherwise.
std::string MoonlightPairDeriveKey(int serverMajorVersion, const std::string& saltHex,
                                   const std::string& pin);

// AES-128-ECB with no padding (the input is zero-padded to a whole block).
std::string MoonlightAesEcb(bool encrypt, const std::string& keyHex, const std::string& dataHex);

// hash(serverChallenge || clientCertSignature || clientSecret), zero-padded to 32
// bytes and AES-ECB encrypted: the serverchallengeresp value.
std::string MoonlightPairChallengeResponse(int serverMajorVersion, const std::string& keyHex,
                                           const std::string& serverChallengeHex,
                                           const std::string& clientCertSignatureHex,
                                           const std::string& clientSecretHex);

// hash(randomChallenge || serverCertSignature || serverSecret), compared by the
// caller against the server's step-2 response to validate the PIN.
std::string MoonlightPairExpectedResponse(int serverMajorVersion,
                                          const std::string& randomChallengeHex,
                                          const std::string& serverCertSignatureHex,
                                          const std::string& serverSecretHex);

// Raw signature bytes of a certificate (hex in, hex out); DER or PEM accepted.
std::string MoonlightCertSignature(const std::string& certHex);

// Signs `clientSecretHex` with the stored client key (SHA256withRSA).
std::string MoonlightSignClientSecret(const std::string& dir, const std::string& clientSecretHex);

// Verifies a server secret against the server certificate (SHA256withRSA/ECDSA).
bool MoonlightVerifyServerSecret(const std::string& serverCertHex,
                                 const std::string& serverSecretHex,
                                 const std::string& serverSignatureHex);

// `length` random bytes as hex (clamped to 1..256).
std::string MoonlightRandomHex(int length);

// The stored client private key re-encoded as PEM, for the TLS client certificate
// on paired requests.
std::string MoonlightClientKeyPem(const std::string& dir);

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_PAIR_H
