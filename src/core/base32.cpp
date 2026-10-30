// Mini USB Bridge - Crockford base32 implementation
// SPDX-License-Identifier: MIT
#include "base32.h"

namespace mub {
namespace {

const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int DecodeChar(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    // Crockford ambiguity mapping.
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    for (int i = 0; i < 32; ++i) {
        if (kAlphabet[i] == c) return i;
    }
    return -1;
}

}  // namespace

std::string Base32Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 8 + 4) / 5);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(acc >> bits) & 0x1F]);
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(acc << (5 - bits)) & 0x1F]);
    return out;
}

bool Base32Decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    int symbols = 0;
    for (char c : text) {
        if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = DecodeChar(c);
        if (v < 0) return false;
        acc = (acc << 5) | static_cast<uint32_t>(v);
        bits += 5;
        ++symbols;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    if (symbols == 0) return false;
    return true;
}

}  // namespace mub
