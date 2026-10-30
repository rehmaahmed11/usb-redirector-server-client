// Mini USB Bridge - SHA-256 / HMAC-SHA256 implementation (FIPS 180-4, RFC 2104)
// SPDX-License-Identifier: MIT
#include "sha256.h"

#include <cstring>

namespace mub {
namespace {

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::Reset() {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    total_ = 0;
    buflen_ = 0;
}

void Sha256::Block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (buflen_ != 0) {
        size_t need = 64 - buflen_;
        size_t take = len < need ? len : need;
        std::memcpy(buf_ + buflen_, p, take);
        buflen_ += take;
        p += take;
        len -= take;
        if (buflen_ == 64) {
            Block(buf_);
            buflen_ = 0;
        }
    }
    while (len >= 64) {
        Block(p);
        p += 64;
        len -= 64;
    }
    if (len != 0) {
        std::memcpy(buf_, p, len);
        buflen_ = len;
    }
}

void Sha256::Finish(uint8_t out[kSha256Len]) {
    const uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (buflen_ != 56) Update(&zero, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; ++i) lenbe[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Update(lenbe, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
    Reset();
}

Sha256Digest Sha256Once(const void* data, size_t len) {
    Sha256 s;
    s.Update(data, len);
    return s.Finish();
}

Sha256Digest Sha256Once(const std::string& s) { return Sha256Once(s.data(), s.size()); }

HmacSha256Ctx::HmacSha256Ctx(const void* key, size_t keylen) {
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (keylen > 64) {
        Sha256Digest d = Sha256Once(key, keylen);
        std::memcpy(k, d.data(), 32);
    } else if (keylen > 0) {
        std::memcpy(k, key, keylen);
    }
    uint8_t ikey[64];
    for (int i = 0; i < 64; ++i) {
        ikey[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        okey_[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    inner_.Update(ikey, sizeof(ikey));
    std::memset(ikey, 0, sizeof(ikey));
}

Sha256Digest HmacSha256Ctx::Finish() {
    Sha256Digest inner = inner_.Finish();
    Sha256 outer;
    outer.Update(okey_, sizeof(okey_));
    outer.Update(inner.data(), inner.size());
    return outer.Finish();
}

Sha256Digest HmacSha256(const void* key, size_t keylen, const void* data, size_t datalen) {
    HmacSha256Ctx ctx(key, keylen);
    ctx.Update(data, datalen);
    return ctx.Finish();
}

bool ConstantTimeEqual(const void* a, const void* b, size_t len) {
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= static_cast<uint8_t>(x[i] ^ y[i]);
    return diff == 0;
}

}  // namespace mub
