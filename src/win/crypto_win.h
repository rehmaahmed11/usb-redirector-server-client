// Mini USB Bridge - Windows cryptographically secure randomness
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mub {

// BCryptGenRandom (system CSPRNG). Returns false only on a broken system.
bool RandomBytes(void* out, size_t len);
std::string RandomHex(size_t bytes);

// Random 64-bit session id (never zero).
uint64_t RandomSessionId();

std::string LocalPCName();
std::string LocalUserName();

}  // namespace mub
