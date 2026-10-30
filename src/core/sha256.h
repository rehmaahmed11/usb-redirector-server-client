// Mini USB Bridge - SHA-256 and HMAC-SHA256 (portable, no external dependency)
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mub {

constexpr size_t kSha256Len = 32;
using Sha256Digest = std::array<uint8_t, kSha256Len>;

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset();
    void Update(const void* data, size_t len);
    void Update(const std::string& s) { Update(s.data(), s.size()); }
    void Finish(uint8_t out[kSha256Len]);
    Sha256Digest Finish() {
        Sha256Digest d{};
        Finish(d.data());
        return d;
    }

private:
    void Block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t total_ = 0;
    uint8_t buf_[64];
    size_t buflen_ = 0;
};

Sha256Digest Sha256Once(const void* data, size_t len);
Sha256Digest Sha256Once(const std::string& s);

// HMAC-SHA256, RFC 2104. key may be any length.
Sha256Digest HmacSha256(const void* key, size_t keylen, const void* data, size_t datalen);
inline Sha256Digest HmacSha256(const std::string& key, const std::string& data) {
    return HmacSha256(key.data(), key.size(), data.data(), data.size());
}
// Incremental helper for concatenated inputs.
class HmacSha256Ctx {
public:
    explicit HmacSha256Ctx(const void* key, size_t keylen);
    void Update(const void* data, size_t len) { inner_.Update(data, len); }
    void Update(const std::string& s) { inner_.Update(s.data(), s.size()); }
    void Update(const uint8_t* p, size_t n) { inner_.Update(p, n); }
    Sha256Digest Finish();

private:
    Sha256 inner_;
    uint8_t okey_[64];
};

// Constant-time comparison; returns true when equal.
bool ConstantTimeEqual(const void* a, const void* b, size_t len);

}  // namespace mub
