// Mini USB Bridge - Crockford base32 (used for pairing codes)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mub {

// Crockford alphabet: digits + consonants, no I, L, O, U (decoder maps I/L->1, O->0).
std::string Base32Encode(const uint8_t* data, size_t len);
inline std::string Base32Encode(const std::vector<uint8_t>& v) {
    return Base32Encode(v.data(), v.size());
}

// Ignores spaces, dashes and case. Returns false on invalid characters or bad length.
bool Base32Decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace mub
