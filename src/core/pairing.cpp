// Mini USB Bridge - pairing code encoding/decoding
// SPDX-License-Identifier: MIT
#include "pairing.h"

#include <cstdio>

#include "base32.h"
#include "bytes.h"
#include "sha256.h"

namespace mub {
namespace {

constexpr const char* kPrefix = "MUB1";

void PushU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

uint32_t ReadU32(const std::vector<uint8_t>& v, size_t off) {
    return (static_cast<uint32_t>(v[off]) << 24) | (static_cast<uint32_t>(v[off + 1]) << 16) |
           (static_cast<uint32_t>(v[off + 2]) << 8) | static_cast<uint32_t>(v[off + 3]);
}

uint16_t ReadU16(const std::vector<uint8_t>& v, size_t off) {
    return static_cast<uint16_t>((v[off] << 8) | v[off + 1]);
}

}  // namespace

std::string PairingCode::Endpoint() const {
    if (!HasDirect()) return "-";
    return host + ":" + std::to_string(directPort);
}

std::string EncodePairingCode(const PairingCode& code) {
    std::string host = code.host.substr(0, kMaxHostLen);
    size_t payloadLen = 1 + 1 + 2 + 4 + kCertPinLen + 8 + kSecretLen + 2 + host.size();
    std::vector<uint8_t> payload;
    payload.reserve(payloadLen);
    payload.push_back(code.version);
    payload.push_back(code.flags);
    payload.push_back(static_cast<uint8_t>(code.directPort >> 8));
    payload.push_back(static_cast<uint8_t>(code.directPort));
    PushU32(payload, code.expiresUnix);
    payload.insert(payload.end(), code.certPin, code.certPin + kCertPinLen);
    for (int i = 7; i >= 0; --i) payload.push_back(static_cast<uint8_t>(code.sessionId >> (8 * i)));
    payload.insert(payload.end(), code.secret, code.secret + kSecretLen);
    payload.push_back(static_cast<uint8_t>(host.size() >> 8));
    payload.push_back(static_cast<uint8_t>(host.size()));
    payload.insert(payload.end(), host.begin(), host.end());

    std::string b32 = Base32Encode(payload);
    std::string out = kPrefix;
    out += '-';
    for (size_t i = 0; i < b32.size(); ++i) {
        if (i != 0 && i % 5 == 0) out += '-';
        out += b32[i];
    }
    return out;
}

bool LooksLikePairingCode(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
        ++i;
    if (text.size() - i < 4) return false;
    char a = text[i], b = text[i + 1], c = text[i + 2], d = text[i + 3];
    auto up = [](char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; };
    return up(a) == 'M' && up(b) == 'U' && up(c) == 'B' && (up(d) == '1');
}

bool DecodePairingCode(const std::string& text, PairingCode& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };

    // Strip whitespace/prefix.
    std::string body;
    body.reserve(text.size());
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
    if (text.size() - i >= 4 && LooksLikePairingCode(text.substr(i))) {
        i += 4;
    } else if (!text.empty()) {
        return fail("Pairing code must start with MUB1");
    }
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') continue;
        body.push_back(c);
    }
    if (body.size() < 60) return fail("Pairing code is too short");

    std::vector<uint8_t> payload;
    if (!Base32Decode(body, payload)) return fail("Pairing code contains an invalid character");
    if (payload.size() < 44) return fail("Pairing code is truncated");

    out = PairingCode{};
    out.version = payload[0];
    if (out.version != kPairVersion) {
        if (error) *error = "Unsupported pairing code version " + std::to_string(out.version);
        return false;
    }
    out.flags = payload[1];
    out.directPort = ReadU16(payload, 2);
    out.expiresUnix = ReadU32(payload, 4);
    std::memcpy(out.certPin, payload.data() + 8, kCertPinLen);
    uint64_t sid = 0;
    for (int k = 0; k < 8; ++k) sid = (sid << 8) | payload[18 + k];
    out.sessionId = sid;
    std::memcpy(out.secret, payload.data() + 26, kSecretLen);
    uint16_t hostLen = ReadU16(payload, 42);
    if (hostLen > kMaxHostLen) return fail("Pairing code host field is invalid");
    if (payload.size() < static_cast<size_t>(44 + hostLen)) return fail("Pairing code host field is truncated");
    out.host.assign(reinterpret_cast<const char*>(payload.data() + 44), hostLen);
    if (out.HasDirect() && out.host.empty()) return fail("Pairing code is missing a server address");
    return true;
}

std::string RelayAuthTag(const PairingCode& code) {
    static const char kLabel[] = "MUB1-RELAY";
    HmacSha256Ctx ctx(code.secret, kSecretLen);
    ctx.Update(kLabel, sizeof(kLabel) - 1);
    uint8_t sid[8];
    for (int i = 0; i < 8; ++i) sid[i] = static_cast<uint8_t>(code.sessionId >> (8 * (7 - i)));
    ctx.Update(sid, sizeof(sid));
    Sha256Digest d = ctx.Finish();
    return std::string(reinterpret_cast<const char*>(d.data()), 16);
}

}  // namespace mub
