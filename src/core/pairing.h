// Mini USB Bridge - pairing code + session key material
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

// Flags carried inside the pairing code.
enum PairingFlags : uint8_t {
    kPairFlagDirect = 0x01,   // code carries a reachable host:port for direct connect
    kPairFlagRelay = 0x02,    // relay endpoint is usable for this session
    kPairFlagLanOnly = 0x04,  // server restricts itself to private/LAN addresses
};

constexpr uint8_t kPairVersion = 1;
constexpr size_t kCertPinLen = 10;    // truncated SHA-256 of the server TLS certificate
constexpr size_t kSecretLen = 16;     // 128-bit session secret (HMAC key)
constexpr size_t kMaxHostLen = 64;

struct PairingCode {
    uint8_t version = kPairVersion;
    uint8_t flags = 0;
    uint16_t directPort = 0;
    uint32_t expiresUnix = 0;  // UTC seconds; 0 = no expiry information
    uint8_t certPin[kCertPinLen] = {};
    uint64_t sessionId = 0;
    uint8_t secret[kSecretLen] = {};
    std::string host;  // empty when the session is relay-only

    bool HasDirect() const { return (flags & kPairFlagDirect) != 0 && directPort != 0 && !host.empty(); }
    bool HasRelay() const { return (flags & kPairFlagRelay) != 0; }
    bool Expired(uint64_t nowUnix) const { return expiresUnix != 0 && nowUnix > expiresUnix; }
    std::string Endpoint() const;  // "host:port" or "-"
};

// Renders as "MUB1-XXXXX-XXXXX-..." (Crockford base32, 5-char groups).
std::string EncodePairingCode(const PairingCode& code);

// Accepts the rendered form, with or without dashes/spaces, any case.
bool DecodePairingCode(const std::string& text, PairingCode& out, std::string* error = nullptr);

// True when the string looks like a pairing code prefix (used for input validation).
bool LooksLikePairingCode(const std::string& text);

// Derives the 32-byte HMAC tag used by the relay to bind a session id to its secret.
std::string RelayAuthTag(const PairingCode& code);

}  // namespace mub
