// Mini USB Bridge - control channel wire protocol
// SPDX-License-Identifier: MIT
//
// Framing (inside the TLS stream):
//   u8  type
//   u16 payload length (big endian, <= 65535)
//   payload
//
// The USB/IP byte stream is NOT framed by this protocol: data channels splice raw
// bytes after a DATA_AUTH / DATA_OK exchange.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bytes.h"
#include "sha256.h"

namespace mub {

constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kMaxFramePayload = 65535;
constexpr size_t kNonceLen = 16;
constexpr size_t kTokenLen = 32;
constexpr size_t kMacLen = 32;

enum class MsgType : uint8_t {
    Hello = 1,        // client -> server  {ver, sessionId, nonceC}
    Challenge = 2,    // server -> client  {nonceS, serverName}
    Auth = 3,         // client -> server  {clientName, mac}
    AuthResult = 4,   // server -> client  {status, mac, token, expiresUnix}
    DataAuth = 5,     // client -> server  {token}
    DataOk = 6,       // server -> client  {status}  (then raw USB/IP bytes both ways)
    DeviceList = 7,   // server -> client  {count, DeviceEntry[]}
    DeviceEvent = 8,  // server -> client  {kind, DeviceEntry}
    RequestAttach = 9,   // client -> server  {busid}
    AttachResult = 10,   // server -> client  {status, message}
    RequestDetach = 11,  // client -> server  {busid}
    DetachResult = 12,   // server -> client  {status, message}
    Ping = 13,           // both              {nonce}
    Pong = 14,           // both              {nonce}
    Bye = 15,            // both              {reason}
    DeviceListRequest = 16,  // client -> server (no payload)
    ResumeAuth = 17,          // client -> server  {token, nonceC} - reconnect after a network drop
};

enum class Status : uint8_t {
    Ok = 0,
    BadVersion = 1,
    UnknownSession = 2,
    Expired = 3,
    BadMac = 4,
    Rejected = 5,
    AlreadyUsed = 6,
    RateLimited = 7,
    NotShared = 8,
    Busy = 9,
    BackendError = 10,
    NotAllowed = 11,
    BadToken = 12,
    Internal = 13,
};

const char* StatusText(Status s);

struct DeviceEntry {
    std::string busid;         // USB/IP bus id, e.g. "1-4" (empty when unknown)
    std::string instanceId;    // Windows device instance id (server side only)
    std::string name;          // friendly name
    std::string manufacturer;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t state = 0;         // 0 = present, 1 = removed (event only)
    uint8_t shared = 0;        // 1 = currently bound/exported
    uint8_t attached = 0;      // 1 = a client has it attached
    uint8_t reserved = 0;
};

struct Message {
    MsgType type = MsgType::Ping;
    std::vector<uint8_t> payload;

    void Encode(std::vector<uint8_t>& out) const;
};

// --- payload codecs -------------------------------------------------------
struct HelloBody {
    uint16_t version = kProtocolVersion;
    uint64_t sessionId = 0;
    uint8_t nonceC[kNonceLen] = {};
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, HelloBody& out);
};

struct ChallengeBody {
    uint8_t nonceS[kNonceLen] = {};
    std::string serverName;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, ChallengeBody& out);
};

struct AuthBody {
    std::string clientName;
    uint8_t mac[kMacLen] = {};
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, AuthBody& out);
};

struct AuthResultBody {
    Status status = Status::Internal;
    uint8_t mac[kMacLen] = {};
    uint8_t token[kTokenLen] = {};
    uint32_t expiresUnix = 0;
    std::string detail;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, AuthResultBody& out);
};

struct DataAuthBody {
    uint8_t token[kTokenLen] = {};
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, DataAuthBody& out);
};

// Reconnect proof: the session token issued after the human approval.
struct ResumeBody {
    uint8_t token[kTokenLen] = {};
    uint8_t nonceC[kNonceLen] = {};
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, ResumeBody& out);
};

struct StatusBody {
    Status status = Status::Internal;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, StatusBody& out);
};

struct ResultBody {
    Status status = Status::Internal;
    std::string busid;   // which device the result refers to
    std::string message;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, ResultBody& out);
};

struct DeviceListBody {
    std::vector<DeviceEntry> devices;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, DeviceListBody& out);
};

struct DeviceEventBody {
    uint8_t kind = 0;  // 0 arrived, 1 removed, 2 changed
    DeviceEntry device;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, DeviceEventBody& out);
};

struct BusidBody {
    std::string busid;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, BusidBody& out);
};

struct NonceBody {
    uint32_t value = 0;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, NonceBody& out);
};

struct ByeBody {
    std::string reason;
    void Write(ByteWriter& w) const;
    static bool Read(ByteReader& r, ByeBody& out);
};

// --- authentication helpers ----------------------------------------------
// Client proof: HMAC(secret, "MUB1-AUTH" || sessionId || nonceC || nonceS || certPin)
Sha256Digest ComputeClientProof(const uint8_t secret[16], uint64_t sessionId, const uint8_t* nonceC,
                                const uint8_t* nonceS, const uint8_t* certPin, size_t pinLen);
// Server proof: HMAC(secret, "MUB1-OK" || sessionId || nonceC || nonceS)
Sha256Digest ComputeServerProof(const uint8_t secret[16], uint64_t sessionId, const uint8_t* nonceC,
                                const uint8_t* nonceS);

// Streaming frame parser. Feed bytes; pop complete messages.
class FrameParser {
public:
    void Reset() { buf_.clear(); }
    // Returns false only on a protocol violation (oversized/invalid frame header).
    bool Feed(const uint8_t* data, size_t len, std::vector<Message>& out, std::string* error = nullptr);

private:
    std::vector<uint8_t> buf_;
};

std::string Hex(const void* data, size_t len);
std::string HexU16(uint16_t v);  // "0e8d"

}  // namespace mub
