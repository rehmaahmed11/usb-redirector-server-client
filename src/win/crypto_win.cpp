// Mini USB Bridge - CSPRNG + machine identity helpers
// SPDX-License-Identifier: MIT
#include "crypto_win.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstring>

#include "../core/protocol.h"  // Hex()
#include "wincompat.h"

namespace mub {

bool RandomBytes(void* out, size_t len) {
#if defined(_WIN32)
    // BCryptGenRandom on Vista+, CryptGenRandom on Windows XP/2000.
    return MubRandomBytes(out, len);
#else
    (void)out;
    (void)len;
    return false;
#endif
}

std::string RandomHex(size_t bytes) {
    std::string hex;
    uint8_t buf[64];
    while (bytes > 0) {
        size_t chunk = bytes > sizeof(buf) ? sizeof(buf) : bytes;
        if (!RandomBytes(buf, chunk)) return {};
        hex += Hex(buf, chunk);
        bytes -= chunk;
    }
    return hex;
}

uint64_t RandomSessionId() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        if (!RandomBytes(&v, sizeof(v))) return 1;
        if (v != 0) return v;
    }
    return 1;
}

std::string LocalPCName() {
#if defined(_WIN32)
    char buf[256] = {};
    DWORD len = sizeof(buf);
    if (!GetComputerNameA(buf, &len)) return "Windows PC";
    return buf;
#else
    return "host";
#endif
}

std::string LocalUserName() {
#if defined(_WIN32)
    char buf[256] = {};
    DWORD len = sizeof(buf);
    if (!GetUserNameA(buf, &len)) return "user";
    return buf;
#else
    return "user";
#endif
}

}  // namespace mub
