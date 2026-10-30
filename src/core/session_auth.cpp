// Mini USB Bridge - pairing decision logic
// SPDX-License-Identifier: MIT
#include "session_auth.h"

#include <cstring>

namespace mub {

bool PairingCodeExhausted(bool codeUsed, uint32_t expiryUnix, int failures, int maxFailures, uint64_t nowUnix) {
    if (codeUsed) return true;
    if (expiryUnix != 0 && nowUnix > expiryUnix) return true;
    if (failures >= maxFailures) return true;
    return false;
}

AuthDecision CheckPairingAttempt(const PairingCode& active, bool codeUsed, uint32_t expiryUnix, int failures,
                                 int maxFailures, const AuthAttempt& attempt, uint64_t nowUnix) {
    AuthDecision d;

    // Any protocol mismatch is a client bug or an attack: refuse, but do not
    // penalise the code (a wrong version is not a guess).
    if (attempt.version != kProtocolVersion) {
        d.status = Status::BadVersion;
        d.detail = "Client protocol version " + std::to_string(attempt.version);
        return d;
    }
    if (failures >= maxFailures) {
        d.status = Status::RateLimited;
        d.detail = "Too many failed attempts - regenerate the pairing code";
        return d;
    }
    if (attempt.sessionId != active.sessionId) {
        d.status = Status::UnknownSession;
        d.detail = "Unknown pairing code";
        d.countFailure = true;
        return d;
    }
    if (codeUsed) {
        d.status = Status::AlreadyUsed;
        d.detail = "This pairing code was already used";
        return d;
    }
    if (expiryUnix != 0 && nowUnix > expiryUnix) {
        d.status = Status::Expired;
        d.detail = "The pairing code expired";
        return d;
    }

    // Constant-time comparison of the HMAC proof.
    Sha256Digest expected = ComputeClientProof(active.secret, active.sessionId, attempt.nonceC, attempt.nonceS,
                                               active.certPin, sizeof(active.certPin));
    if (!ConstantTimeEqual(expected.data(), attempt.mac, kMacLen)) {
        d.status = Status::BadMac;
        d.detail = "Invalid authentication proof";
        d.countFailure = true;
        return d;
    }

    d.status = Status::Ok;
    d.detail = "Approved";
    return d;
}

}  // namespace mub
