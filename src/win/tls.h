// Mini USB Bridge - TLS 1.2/1.3 via SChannel (with certificate pinning)
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <schannel.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

// Server identity: self-signed certificate + private key, stored as a
// DPAPI-protected PFX. The truncated SHA-256 of the DER ("pin") travels inside
// the pairing code, so the client can refuse any other server certificate.
struct TlsIdentity {
    std::vector<uint8_t> pfx;
    std::string password;
    std::vector<uint8_t> certDer;
    uint8_t pin[10] = {};
    std::string subject;
};

bool CreateSelfSignedIdentity(TlsIdentity& id, std::string* err);
bool SaveIdentity(const TlsIdentity& id, const std::string& path, std::string* err);
// Loads the identity at 'path'; creates and saves it when missing or unreadable.
bool LoadOrCreateIdentity(TlsIdentity& id, const std::string& path, std::string* err);

#if defined(_WIN32)
class TlsChannel {
public:
    TlsChannel() = default;
    ~TlsChannel();
    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    // Takes ownership of 'sock' (closed on Close/destructor).
    bool ServerHandshake(SOCKET sock, const TlsIdentity& id, std::string* err);
    // Verifies the peer certificate against 'pin' (10 bytes). Fails on mismatch.
    bool ClientHandshake(SOCKET sock, const uint8_t pin[10], std::string* err);

    int Read(void* buf, int len);                    // >0 bytes, 0 closed, -1 error
    bool Write(const void* buf, size_t len);         // false on failure
    void Close();

    SOCKET Socket() const { return sock_; }
    bool Valid() const { return haveCtx_ && !failed_; }
    std::string PeerCertPin() const { return peerPin_; }
    std::string CipherInfo() const { return cipher_; }

private:
    bool Handshake(const TlsIdentity* serverId, const uint8_t* pin, bool server, std::string* err);
    bool PumpHandshake(PCtxtHandle ctx, const void* in, size_t inLen, bool server, std::string* err);
    void SetError(const std::string& what);
    bool sspiError(SECURITY_STATUS st, const char* where, std::string* err);

    SOCKET sock_ = INVALID_SOCKET;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false;
    bool haveCtx_ = false;
    bool failed_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<uint8_t> in_;       // raw TLS bytes not yet consumed
    size_t inOff_ = 0;
    std::vector<uint8_t> plain_;    // decrypted bytes waiting for the caller
    size_t plainOff_ = 0;
    std::string peerPin_;
    std::string cipher_;
};
#endif

}  // namespace mub
