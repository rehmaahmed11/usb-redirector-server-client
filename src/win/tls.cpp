// Mini USB Bridge - SChannel TLS implementation
// SPDX-License-Identifier: MIT
#include "tls.h"

#if defined(_WIN32)

#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <fstream>

#include "../core/logger.h"
#include "../core/os_support.h"
#include "../core/protocol.h"  // Hex()
#include "../core/settings.h"
#include "../core/sha256.h"
#include "../core/strings.h"
#include "crypto_win.h"
#include "net.h"
#include "wincompat.h"

// Protocol flags: present in every SDK from Vista on, re-declared here with the
// documented values so the Windows XP toolset (SDK 7.1) can build the same code.
#ifndef SP_PROT_TLS1_0_CLIENT
#define SP_PROT_TLS1_0_CLIENT 0x00000080
#endif
#ifndef SP_PROT_TLS1_0_SERVER
#define SP_PROT_TLS1_0_SERVER 0x00000040
#endif
#ifndef SP_PROT_TLS1_1_CLIENT
#define SP_PROT_TLS1_1_CLIENT 0x00000200
#endif
#ifndef SP_PROT_TLS1_1_SERVER
#define SP_PROT_TLS1_1_SERVER 0x00000100
#endif
#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x00000800
#endif
#ifndef SP_PROT_TLS1_2_SERVER
#define SP_PROT_TLS1_2_SERVER 0x00000400
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SP_PROT_TLS1_3_SERVER
#define SP_PROT_TLS1_3_SERVER 0x00001000
#endif

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace mub {
namespace {

constexpr wchar_t kProvName[] = L"Microsoft Enhanced Cryptographic Provider v1.0";

std::wstring AsciiToWide(const std::string& s) { return Utf8ToWide(s); }

// Turns the portable TLS policy (src/core/os_support.cpp) into SChannel's
// grbitEnabledProtocols mask for the current side of the connection.
DWORD ProtocolMaskFor(const TlsProfile& tls, bool server) {
    DWORD mask = 0;
    if (tls.offered & TlsProfile::kTls10) mask |= server ? SP_PROT_TLS1_0_SERVER : SP_PROT_TLS1_0_CLIENT;
    if (tls.offered & TlsProfile::kTls11) mask |= server ? SP_PROT_TLS1_1_SERVER : SP_PROT_TLS1_1_CLIENT;
    if (tls.offered & TlsProfile::kTls12) mask |= server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
    if (tls.offered & TlsProfile::kTls13) mask |= server ? SP_PROT_TLS1_3_SERVER : SP_PROT_TLS1_3_CLIENT;
    return mask;
}

const char* ProtocolName(DWORD protocol) {
    switch (protocol) {
        case 0x00000002: return "SSL 2.0";
        case 0x00000004: return "SSL 3.0";
        case 0x00000008: return "TLS 1.0";
        case 0x00000010: return "TLS 1.1";
        case 0x00000020: return "TLS 1.2";
        case 0x00000040: return "TLS 1.3";
        default: return "unknown protocol";
    }
}

TlsProfile CurrentTlsPolicy() { return TlsProfileFor(OsInfo(), GetSettings().allowLegacyTls); }

bool GenPassword(std::string& out) {
    uint8_t raw[16];
    if (!RandomBytes(raw, sizeof(raw))) return false;
    out = Hex(raw, sizeof(raw));
    return true;
}

std::string ErrText(DWORD code) {
    LPSTR buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string s = "error " + std::to_string(code);
    if (n && buf) {
        std::string msg(buf, n);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
        s += " (" + msg + ")";
    }
    if (buf) LocalFree(buf);
    return s;
}

}  // namespace

bool CreateSelfSignedIdentity(TlsIdentity& id, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        LogE("tls", m);
        return false;
    };

    std::string password;
    if (!GenPassword(password)) return fail("Cannot generate certificate key password");
    std::wstring wpassword = AsciiToWide(password);

    HCRYPTPROV prov = 0;
    std::wstring container = L"MiniUsbBridge-" + AsciiToWide(Hex(reinterpret_cast<const void*>(password.data()), 4));
    if (!CryptAcquireContextW(&prov, container.c_str(), kProvName, PROV_RSA_FULL, CRYPT_NEWKEYSET)) {
        return fail("CryptAcquireContext failed: " + ErrText(GetLastError()));
    }
    HCRYPTKEY key = 0;
    if (!CryptGenKey(prov, AT_SIGNATURE, (2048u << 16) | CRYPT_EXPORTABLE, &key)) {
        DWORD e = GetLastError();
        CryptReleaseContext(prov, 0);
        return fail("CryptGenKey failed: " + ErrText(e));
    }

    std::string cn = "Mini USB Bridge " + LocalPCName();
    std::string subjectStr = "CN=" + cn + ", O=Mini USB Bridge, C=US";
    std::wstring wsubject = AsciiToWide(subjectStr);

    CERT_NAME_BLOB subject{};
    DWORD cb = 0;
    if (!CertStrToNameW(X509_ASN_ENCODING, wsubject.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &cb, nullptr) ||
        cb == 0) {
        DWORD e = GetLastError();
        CryptDestroyKey(key);
        CryptReleaseContext(prov, 0);
        return fail("CertStrToName failed: " + ErrText(e));
    }
    std::vector<uint8_t> subjectBuf(cb);
    subject.cbData = cb;
    subject.pbData = subjectBuf.data();
    if (!CertStrToNameW(X509_ASN_ENCODING, wsubject.c_str(), CERT_X500_NAME_STR, nullptr, subject.pbData,
                        &subject.cbData, nullptr)) {
        DWORD e = GetLastError();
        CryptDestroyKey(key);
        CryptReleaseContext(prov, 0);
        return fail("CertStrToName (encode) failed: " + ErrText(e));
    }

    CRYPT_KEY_PROV_INFO keyProv{};
    keyProv.pwszContainerName = const_cast<LPWSTR>(container.c_str());
    keyProv.pwszProvName = const_cast<LPWSTR>(kProvName);
    keyProv.dwProvType = PROV_RSA_FULL;
    keyProv.dwKeySpec = AT_SIGNATURE;

    SYSTEMTIME endTime{};
    GetSystemTime(&endTime);
    endTime.wYear = static_cast<WORD>(endTime.wYear + 10);  // 10 year validity

    // Windows XP's SChannel only understands SHA-1 signatures, so an identity
    // created on XP is signed with SHA-1; everything newer gets SHA-256.
    CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
    const TlsProfile tls = CurrentTlsPolicy();
    sigAlg.pszObjId = const_cast<LPSTR>(tls.sha1Certificate ? szOID_OIWSEC_sha1 : szOID_NIST_sha256);
    LogI("tls", std::string("Creating server identity, certificate signature ") +
                   (tls.sha1Certificate ? "SHA-1 (legacy SChannel)" : "SHA-256"));

    PCCERT_CONTEXT cert =
        CertCreateSelfSignCertificate(prov, &subject, 0, &keyProv, &sigAlg, nullptr, &endTime, nullptr);
    if (!cert) {
        DWORD e = GetLastError();
        CryptDestroyKey(key);
        CryptReleaseContext(prov, 0);
        return fail("CertCreateSelfSignCertificate failed: " + ErrText(e));
    }

    // Export to PFX (needs an exportable private key).
    bool ok = false;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (store) {
        if (CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            CRYPT_DATA_BLOB pfx{};
            if (PFXExportCertStoreEx(store, &pfx, wpassword.c_str(), nullptr,
                                     EXPORT_PRIVATE_KEYS | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY) &&
                pfx.cbData > 0) {
                std::vector<uint8_t> blob(pfx.cbData);
                pfx.pbData = blob.data();
                if (PFXExportCertStoreEx(store, &pfx, wpassword.c_str(), nullptr,
                                         EXPORT_PRIVATE_KEYS | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY)) {
                    id.pfx.assign(blob.begin(), blob.begin() + pfx.cbData);
                    id.password = password;
                    id.certDer.assign(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
                    Sha256Digest d = Sha256Once(id.certDer.data(), id.certDer.size());
                    std::memcpy(id.pin, d.data(), sizeof(id.pin));
                    id.subject = subjectStr;
                    ok = true;
                }
            }
        }
        CertCloseStore(store, 0);
    }
    if (!ok) {
        DWORD e = GetLastError();
        CertFreeCertificateContext(cert);
        CryptDestroyKey(key);
        CryptReleaseContext(prov, 0);
        return fail("PFX export failed: " + ErrText(e) +
                    " - the certificate could not be stored, file sharing features stay disabled");
    }
    CertFreeCertificateContext(cert);
    CryptDestroyKey(key);
    CryptReleaseContext(prov, 0);
    if (err) err->clear();
    LogI("tls", "Generated self-signed identity " + cn + " pin=" + Hex(id.pin, sizeof(id.pin)));
    return true;
}

namespace {
bool SerializeIdentity(const TlsIdentity& id, std::vector<uint8_t>& out) {
    if (id.password.size() > 63 || id.pfx.empty()) return false;
    out.clear();
    out.push_back(static_cast<uint8_t>(id.password.size() >> 8));
    out.push_back(static_cast<uint8_t>(id.password.size()));
    out.insert(out.end(), id.password.begin(), id.password.end());
    uint32_t n = static_cast<uint32_t>(id.pfx.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
    out.insert(out.end(), id.pfx.begin(), id.pfx.end());
    return true;
}

bool DeserializeIdentity(const std::vector<uint8_t>& in, TlsIdentity& id) {
    if (in.size() < 6) return false;
    size_t pwLen = (static_cast<size_t>(in[0]) << 8) | in[1];
    if (pwLen > 63 || in.size() < 2 + pwLen + 4) return false;
    id.password.assign(reinterpret_cast<const char*>(in.data() + 2), pwLen);
    size_t off = 2 + pwLen;
    uint32_t n = (static_cast<uint32_t>(in[off]) << 24) | (static_cast<uint32_t>(in[off + 1]) << 16) |
                 (static_cast<uint32_t>(in[off + 2]) << 8) | in[off + 3];
    off += 4;
    if (n == 0 || in.size() < off + n) return false;
    id.pfx.assign(in.begin() + off, in.begin() + off + n);
    return true;
}

bool DerivePinFromPfx(TlsIdentity& id) {
    CRYPT_DATA_BLOB blob{};
    blob.cbData = static_cast<DWORD>(id.pfx.size());
    blob.pbData = const_cast<BYTE*>(id.pfx.data());
    std::wstring pw = AsciiToWide(id.password);
    HCERTSTORE store = PFXImportCertStore(&blob, pw.c_str(), CRYPT_EXPORTABLE);
    if (!store) return false;
    bool ok = false;
    PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                     CERT_FIND_ANY, nullptr, nullptr);
    if (cert) {
        id.certDer.assign(cert->pbCertEncoded, cert->pbCertEncoded + cert->cbCertEncoded);
        Sha256Digest d = Sha256Once(id.certDer.data(), id.certDer.size());
        std::memcpy(id.pin, d.data(), sizeof(id.pin));
        ok = true;
        CertFreeCertificateContext(cert);
    }
    CertCloseStore(store, 0);
    return ok;
}

}  // namespace

bool SaveIdentity(const TlsIdentity& id, const std::string& path, std::string* err) {
    std::vector<uint8_t> plain;
    if (!SerializeIdentity(id, plain)) {
        if (err) *err = "Identity is incomplete";
        return false;
    }
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(plain.size());
    in.pbData = plain.data();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Mini USB Bridge identity", nullptr, nullptr, nullptr, 0, &out)) {
        if (err) *err = "CryptProtectData failed: " + ErrText(GetLastError());
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    bool ok = static_cast<bool>(f);
    if (ok) f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    f.close();
    LocalFree(out.pbData);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok && err) *err = "Cannot write " + path;
    return ok;
}

bool LoadOrCreateIdentity(TlsIdentity& id, const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!blob.empty()) {
            DATA_BLOB in{};
            in.cbData = static_cast<DWORD>(blob.size());
            in.pbData = blob.data();
            DATA_BLOB out{};
            if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
                std::vector<uint8_t> plain(out.pbData, out.pbData + out.cbData);
                LocalFree(out.pbData);
                TlsIdentity loaded;
                if (DeserializeIdentity(plain, loaded) && DerivePinFromPfx(loaded)) {
                    id = loaded;
                    if (err) err->clear();
                    LogI("tls", "Loaded TLS identity from " + path);
                    return true;
                }
                LogW("tls", "Stored identity is unusable; generating a new one");
            } else {
                LogW("tls", "Stored identity could not be decrypted; generating a new one");
            }
        }
    }
    TlsIdentity fresh;
    if (!CreateSelfSignedIdentity(fresh, err)) return false;
    std::string saveErr;
    if (!SaveIdentity(fresh, path, &saveErr)) {
        LogW("tls", "Identity not persisted: " + saveErr);
        if (err) err->clear();  // usable for this run, just not persistent
    }
    id = fresh;
    return true;
}

// ---------------------------------------------------------------------------

TlsChannel::~TlsChannel() { Close(); }

void TlsChannel::Close() {
    // The MUB protocol sends its own BYE on the control channel, so an abrupt
    // TLS teardown is safe here and avoids half-closed-state pitfalls.
    if (haveCtx_) {
        DeleteSecurityContext(&ctx_);
        haveCtx_ = false;
    }
    if (haveCred_) {
        FreeCredentialsHandle(&cred_);
        haveCred_ = false;
    }
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    in_.clear();
    plain_.clear();
    inOff_ = 0;
    plainOff_ = 0;
}

bool TlsChannel::sspiError(SECURITY_STATUS st, const char* where, std::string* err) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(st));
    std::string msg = std::string("TLS ") + where + " failed: " + code;
    if (err) *err = msg;
    LogE("tls", msg);
    failed_ = true;
    return false;
}

bool TlsChannel::Handshake(const TlsIdentity* serverId, const uint8_t* pin, bool server, std::string* err) {
    // Protocol set and cipher policy depend on this Windows version (XP only has
    // TLS 1.0 and no AES/strong-crypto flag; Windows 10 offers up to TLS 1.2
    // through the SCHANNEL_CRED structure used here).
    const TlsProfile tls = CurrentTlsPolicy();
    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    // 0 = "use the operating system policy" (respects registry hardening on
    // Vista+); an explicit mask is only used where the OS default is not enough.
    cred.grbitEnabledProtocols = tls.useSystemDefault ? 0 : ProtocolMaskFor(tls, server);
    cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    if (tls.strongCrypto) cred.dwFlags |= SCH_USE_STRONG_CRYPTO;
    LogI("tls", std::string("TLS policy: ") + tls.description + " (mask 0x" + Hex(&cred.grbitEnabledProtocols, 4) + ")");
    HCERTSTORE pfxStore = nullptr;
    PCCERT_CONTEXT certCtx = nullptr;

    if (server) {
        if (!serverId) {
            if (err) *err = "No server identity";
            return false;
        }
        CRYPT_DATA_BLOB blob{};
        blob.cbData = static_cast<DWORD>(serverId->pfx.size());
        blob.pbData = const_cast<BYTE*>(serverId->pfx.data());
        std::wstring pw = AsciiToWide(serverId->password);
        pfxStore = PFXImportCertStore(&blob, pw.c_str(), CRYPT_EXPORTABLE);
        if (!pfxStore) {
            return sspiError(static_cast<SECURITY_STATUS>(GetLastError()), "PFXImportCertStore", err);
        }
        certCtx = CertFindCertificateInStore(pfxStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_ANY,
                                             nullptr, nullptr);
        if (!certCtx) {
            CertCloseStore(pfxStore, 0);
            if (err) *err = "Server certificate could not be loaded from the PFX";
            failed_ = true;
            return false;
        }
        cred.cCreds = 1;
        cred.paCred = &certCtx;
    }

    TimeStamp expiry{};
    SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
                                                   server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr,
                                                   &cred, nullptr, nullptr, &cred_, &expiry);
    if (certCtx) CertFreeCertificateContext(certCtx);
    if (pfxStore) CertCloseStore(pfxStore, 0);
    if (st != SEC_E_OK) return sspiError(st, "AcquireCredentialsHandle", err);
    haveCred_ = true;

    if (!PumpHandshake(nullptr, nullptr, 0, server, err)) return false;

    if (!server && pin) {
        PCCERT_CONTEXT remote = nullptr;
        if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) == SEC_E_OK && remote) {
            Sha256Digest d = Sha256Once(remote->pbCertEncoded, remote->cbCertEncoded);
            peerPin_ = Hex(d.data(), 10);
            if (!ConstantTimeEqual(d.data(), pin, 10)) {
                CertFreeCertificateContext(remote);
                if (err) {
                    *err = "Server certificate does not match the pairing code (possible interception)";
                }
                LogE("tls", "Certificate pin mismatch: got " + peerPin_);
                failed_ = true;
                return false;
            }
            CertFreeCertificateContext(remote);
        } else {
            if (err) *err = "Server presented no certificate";
            failed_ = true;
            return false;
        }
    }

    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
        return sspiError(SEC_E_INTERNAL_ERROR, "QueryContextAttributes(STREAM_SIZES)", err);
    }
    SecPkgContext_CipherInfo ci{};
    ci.dwVersion = SECPKG_ATTR_CIPHER_INFO;
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_CIPHER_INFO, &ci) == SEC_E_OK) {
        cipher_ = WideToUtf8(ci.szCipherSuite);
    }
    SecPkgContext_ConnectionInfo conn{};
    if (QueryContextAttributesW(&ctx_, SECPKG_ATTR_CONNECTION_INFO, &conn) == SEC_E_OK) {
        LogI("tls", std::string(server ? "server" : "client") + " negotiated " + ProtocolName(conn.dwProtocol) +
                       " / " + cipher_);
        if (conn.dwProtocol <= 0x00000008) {
            LogW("tls", "This session uses TLS 1.0 because one side runs a legacy Windows version; "
                        "upgrade it for TLS 1.2");
        }
    }
    in_.clear();
    plain_.clear();
    inOff_ = 0;
    plainOff_ = 0;
    return true;
}

bool TlsChannel::PumpHandshake(PCtxtHandle existing, const void* firstInput, size_t firstLen, bool server,
                               std::string* err) {
    (void)existing;
    std::vector<uint8_t> token;
    if (firstLen && firstInput) {
        const uint8_t* p = static_cast<const uint8_t*>(firstInput);
        token.assign(p, p + firstLen);
    }
    bool first = true;
    for (int iter = 0; iter < 12; ++iter) {
        SecBuffer inBufs[2];
        SecBufferDesc inDesc{SECBUFFER_VERSION, 0, inBufs};
        if (!token.empty()) {
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[0].pvBuffer = token.data();
            inBufs[0].cbBuffer = static_cast<unsigned long>(token.size());
            inDesc.cBuffers = 1;
        }
        SecBuffer outBufs[1];
        outBufs[0].BufferType = SECBUFFER_TOKEN;
        outBufs[0].pvBuffer = nullptr;
        outBufs[0].cbBuffer = 0;
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, outBufs};

        unsigned long outFlags = 0;
        SECURITY_STATUS st;
        const bool haveExisting = haveCtx_;

        if (server) {
            unsigned long flags = ASC_REQ_STREAM | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_SEQUENCE_DETECT |
                                  ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_INTEGRITY |
                                  ASC_REQ_EXTENDED_ERROR;
            st = AcceptSecurityContext(&cred_, haveExisting ? &ctx_ : nullptr, token.empty() ? nullptr : &inDesc,
                                       flags, SECURITY_NATIVE_DREP, &ctx_, &outDesc, &outFlags, nullptr);
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) haveCtx_ = true;
        } else {
            unsigned long flags = ISC_REQ_STREAM | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_SEQUENCE_DETECT |
                                  ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY |
                                  ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION;
            st = InitializeSecurityContextW(&cred_, haveExisting ? &ctx_ : nullptr, nullptr, flags, 0,
                                            SECURITY_NATIVE_DREP, token.empty() ? nullptr : &inDesc, 0, &ctx_,
                                            &outDesc, &outFlags, nullptr);
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) haveCtx_ = true;
        }

        if (outBufs[0].cbBuffer && outBufs[0].pvBuffer) {
            bool sent = SendAll(sock_, outBufs[0].pvBuffer, outBufs[0].cbBuffer);
            FreeContextBuffer(outBufs[0].pvBuffer);
            if (!sent) return sspiError(st, "handshake write", err);
        }
        first = false;

        if (st == SEC_E_OK) return true;
        if (st == SEC_I_CONTINUE_NEEDED) {
            // Read the peer's next token (blocking with the socket's receive timeout).
            uint8_t buf[0x4000];
            int n = RecvSome(sock_, buf, sizeof(buf));
            if (n <= 0) {
                if (err) *err = "TLS handshake ended unexpectedly (peer closed)";
                failed_ = true;
                return false;
            }
            token.assign(buf, buf + n);
            continue;
        }
        return sspiError(st, server ? "AcceptSecurityContext" : "InitializeSecurityContext", err);
    }
    if (err) *err = "TLS handshake did not converge";
    failed_ = true;
    return false;
}

bool TlsChannel::ServerHandshake(SOCKET sock, const TlsIdentity& id, std::string* err) {
    sock_ = sock;
    SetRecvTimeout(sock_, 15000);
    SetSendTimeout(sock_, 15000);
    SetNoDelay(sock_);
    if (!Handshake(&id, nullptr, true, err)) {
        Close();
        return false;
    }
    SetRecvTimeout(sock_, 0);  // blocking
    return true;
}

bool TlsChannel::ClientHandshake(SOCKET sock, const uint8_t pin[10], std::string* err) {
    sock_ = sock;
    SetRecvTimeout(sock_, 15000);
    SetSendTimeout(sock_, 15000);
    SetNoDelay(sock_);
    if (!Handshake(nullptr, pin, false, err)) {
        Close();
        return false;
    }
    SetRecvTimeout(sock_, 0);
    return true;
}

int TlsChannel::Read(void* buf, int len) {
    if (failed_ || !haveCtx_ || len <= 0) return -1;
    if (plainOff_ < plain_.size()) {
        size_t n = plain_.size() - plainOff_;
        if (n > static_cast<size_t>(len)) n = static_cast<size_t>(len);
        std::memcpy(buf, plain_.data() + plainOff_, n);
        plainOff_ += n;
        if (plainOff_ >= plain_.size()) {
            plain_.clear();
            plainOff_ = 0;
        }
        return static_cast<int>(n);
    }

    for (int attempt = 0; attempt < 64; ++attempt) {
        if (in_.size() - inOff_ == 0) {
            in_.resize(static_cast<size_t>(sizes_.cbHeader + sizes_.cbMaximumMessage + sizes_.cbTrailer));
            inOff_ = 0;
            int n = RecvSome(sock_, in_.data(), static_cast<int>(in_.size()));
            if (n <= 0) {
                if (n < 0) {
                    int code = WSAGetLastError();
                    if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) return -2;  // no data yet
                }
                failed_ = true;
                return n == 0 ? 0 : -1;
            }
            in_.resize(static_cast<size_t>(n));
        }

        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].pvBuffer = in_.data() + inOff_;
        bufs[0].cbBuffer = static_cast<unsigned long>(in_.size() - inOff_);
        for (int i = 1; i < 4; ++i) {
            bufs[i].BufferType = SECBUFFER_EMPTY;
            bufs[i].pvBuffer = nullptr;
            bufs[i].cbBuffer = 0;
        }
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
        SECURITY_STATUS st = DecryptMessage(&ctx_, &desc, 0, nullptr);

        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            // Grow the buffer and read more.
            size_t used = in_.size() - inOff_;
            std::vector<uint8_t> more(used + sizes_.cbMaximumMessage + sizes_.cbHeader + sizes_.cbTrailer);
            std::memcpy(more.data(), in_.data() + inOff_, used);
            int n = RecvSome(sock_, more.data() + used, static_cast<int>(more.size() - used));
            if (n <= 0) {
                if (n < 0) {
                    int code = WSAGetLastError();
                    if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) return -2;
                }
                failed_ = true;
                return n == 0 ? 0 : -1;
            }
            more.resize(used + static_cast<size_t>(n));
            in_.swap(more);
            inOff_ = 0;
            continue;
        }
        if (st == SEC_I_CONTEXT_EXPIRED) {
            failed_ = true;
            return 0;
        }
        if (st == SEC_I_RENEGOTIATE) {
            LogW("tls", "Peer requested renegotiation; closing the channel");
            failed_ = true;
            return -1;
        }
        if (st != SEC_E_OK) {
            failed_ = true;
            return -1;
        }

        size_t extraOff = 0;
        size_t extraLen = 0;
        for (int i = 0; i < 4; ++i) {
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                const uint8_t* p = static_cast<const uint8_t*>(bufs[i].pvBuffer);
                plain_.assign(p, p + bufs[i].cbBuffer);
                plainOff_ = 0;
            } else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
                extraOff = static_cast<size_t>(static_cast<const uint8_t*>(bufs[i].pvBuffer) - in_.data());
                extraLen = bufs[i].cbBuffer;
            }
        }
        if (extraLen > 0) {
            std::memmove(in_.data(), in_.data() + extraOff, extraLen);
            in_.resize(extraLen);
            inOff_ = 0;
        } else {
            in_.clear();
            inOff_ = 0;
        }
        if (!plain_.empty()) return Read(buf, len);
    }
    return -1;
}

bool TlsChannel::Write(const void* buf, size_t len) {
    if (failed_ || !haveCtx_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > sizes_.cbMaximumMessage) chunk = sizes_.cbMaximumMessage;
        std::vector<uint8_t> frame(sizes_.cbHeader + chunk + sizes_.cbTrailer);
        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = frame.data();
        bufs[0].cbBuffer = sizes_.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = frame.data() + sizes_.cbHeader;
        bufs[1].cbBuffer = static_cast<unsigned long>(chunk);
        std::memcpy(bufs[1].pvBuffer, p + off, chunk);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = frame.data() + sizes_.cbHeader + chunk;
        bufs[2].cbBuffer = sizes_.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        bufs[3].pvBuffer = nullptr;
        bufs[3].cbBuffer = 0;
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
        if (EncryptMessage(&ctx_, 0, &desc, 0) != SEC_E_OK) {
            failed_ = true;
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (bufs[i].cbBuffer && bufs[i].pvBuffer) {
                if (!SendAll(sock_, bufs[i].pvBuffer, bufs[i].cbBuffer)) {
                    failed_ = true;
                    return false;
                }
            }
        }
        off += chunk;
    }
    return true;
}

}  // namespace mub

#endif  // _WIN32
