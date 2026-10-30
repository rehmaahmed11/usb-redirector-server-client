// Mini USB Bridge - pairing decision logic (pure, unit tested)
// SPDX-License-Identifier: MIT
//
// The server's acceptance decision is isolated here so that every rejection
// path (version, unknown session, reuse, expiry, rate limit, bad proof) can be
// exercised by tests without sockets or TLS.
#pragma once

#include <cstdint>

#include "pairing.h"
#include "protocol.h"

namespace mub {

constexpr int kDefaultMaxAuthFailures = 5;

struct AuthAttempt {
    uint16_t version = kProtocolVersion;
    uint64_t sessionId = 0;
    uint8_t nonceC[kNonceLen] = {};
    uint8_t nonceS[kNonceLen] = {};
    uint8_t mac[kMacLen] = {};
};

struct AuthDecision {
    Status status = Status::Internal;
    std::string detail;
    bool countFailure = false;  // increment the failure counter
    bool ok() const { return status == Status::Ok; }
};

// Checks a complete pairing attempt. 'failures' is the number of failed attempts
// seen so far for this code.
AuthDecision CheckPairingAttempt(const PairingCode& active, bool codeUsed, uint32_t expiryUnix,
                                 int failures, int maxFailures, const AuthAttempt& attempt, uint64_t nowUnix);

// True when the code must no longer be accepted for a *new* pairing.
bool PairingCodeExhausted(bool codeUsed, uint32_t expiryUnix, int failures, int maxFailures, uint64_t nowUnix);

}  // namespace mub
