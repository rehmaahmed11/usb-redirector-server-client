// Mini USB Bridge - portable byte buffer helpers
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mub {

// Big-endian writer. Every multi-byte value in the MUB protocol is big-endian.
class ByteWriter {
public:
    std::vector<uint8_t> buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v));
    }
    void u32(uint32_t v) {
        for (int i = 3; i >= 0; --i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void raw(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), p, p + len);
    }
    // Length-prefixed UTF-8 string (u16 length + bytes). Truncated to 65535.
    void str(const std::string& s) {
        size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<uint16_t>(n));
        raw(s.data(), n);
    }
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t len) : p_(data), n_(len) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return n_ - pos_; }
    size_t position() const { return pos_; }

    uint8_t u8() {
        if (!need(1)) return 0;
        return p_[pos_++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>((p_[pos_] << 8) | p_[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | p_[pos_ + i];
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p_[pos_ + i];
        pos_ += 8;
        return v;
    }
    bool raw(void* out, size_t len) {
        if (!need(len)) return false;
        std::memcpy(out, p_ + pos_, len);
        pos_ += len;
        return true;
    }
    std::string str() {
        uint16_t n = u16();
        if (!ok() || !need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p_ + pos_), n);
        pos_ += n;
        return s;
    }

private:
    bool need(size_t k) {
        if (!ok_ || pos_ + k > n_) {
            ok_ = false;
            return false;
        }
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace mub
