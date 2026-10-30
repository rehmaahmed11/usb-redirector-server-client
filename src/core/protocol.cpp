// Mini USB Bridge - control channel wire protocol implementation
// SPDX-License-Identifier: MIT
#include "protocol.h"

#include <cstdio>
#include <cstring>

namespace mub {

const char* StatusText(Status s) {
    switch (s) {
        case Status::Ok: return "OK";
        case Status::BadVersion: return "Protocol version mismatch";
        case Status::UnknownSession: return "Unknown or already used pairing code";
        case Status::Expired: return "Pairing code expired";
        case Status::BadMac: return "Authentication failed";
        case Status::Rejected: return "Request rejected by the server user";
        case Status::AlreadyUsed: return "Pairing code was already used";
        case Status::RateLimited: return "Too many failed attempts";
        case Status::NotShared: return "Device is not shared";
        case Status::Busy: return "Device is busy";
        case Status::BackendError: return "USB/IP backend error";
        case Status::NotAllowed: return "Not allowed";
        case Status::BadToken: return "Session token rejected";
        case Status::Internal: return "Internal error";
    }
    return "Unknown";
}

std::string Hex(const void* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0xF]);
    }
    return s;
}

std::string HexU16(uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return Hex(b, 2);
}

void Message::Encode(std::vector<uint8_t>& out) const {
    size_t n = payload.size() > kMaxFramePayload ? kMaxFramePayload : payload.size();
    out.clear();
    out.reserve(n + 3);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), payload.begin(), payload.begin() + n);
}

void HelloBody::Write(ByteWriter& w) const {
    w.u16(version);
    w.u64(sessionId);
    w.raw(nonceC, kNonceLen);
}
bool HelloBody::Read(ByteReader& r, HelloBody& out) {
    out.version = r.u16();
    out.sessionId = r.u64();
    if (!r.raw(out.nonceC, kNonceLen)) return false;
    return true;
}

void ChallengeBody::Write(ByteWriter& w) const {
    w.raw(nonceS, kNonceLen);
    w.str(serverName);
}
bool ChallengeBody::Read(ByteReader& r, ChallengeBody& out) {
    if (!r.raw(out.nonceS, kNonceLen)) return false;
    out.serverName = r.str();
    return r.ok();
}

void AuthBody::Write(ByteWriter& w) const {
    w.str(clientName);
    w.raw(mac, kMacLen);
}
bool AuthBody::Read(ByteReader& r, AuthBody& out) {
    out.clientName = r.str();
    if (!r.raw(out.mac, kMacLen)) return false;
    return true;
}

void AuthResultBody::Write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(status));
    w.raw(mac, kMacLen);
    w.raw(token, kTokenLen);
    w.u32(expiresUnix);
    w.str(detail);
}
bool AuthResultBody::Read(ByteReader& r, AuthResultBody& out) {
    out.status = static_cast<Status>(r.u8());
    if (!r.raw(out.mac, kMacLen)) return false;
    if (!r.raw(out.token, kTokenLen)) return false;
    out.expiresUnix = r.u32();
    out.detail = r.str();
    return r.ok();
}

void ResumeBody::Write(ByteWriter& w) const {
    w.raw(token, kTokenLen);
    w.raw(nonceC, kNonceLen);
}
bool ResumeBody::Read(ByteReader& r, ResumeBody& out) {
    if (!r.raw(out.token, kTokenLen)) return false;
    return r.raw(out.nonceC, kNonceLen);
}

void DataAuthBody::Write(ByteWriter& w) const { w.raw(token, kTokenLen); }
bool DataAuthBody::Read(ByteReader& r, DataAuthBody& out) { return r.raw(out.token, kTokenLen); }

void StatusBody::Write(ByteWriter& w) const { w.u8(static_cast<uint8_t>(status)); }
bool StatusBody::Read(ByteReader& r, StatusBody& out) {
    out.status = static_cast<Status>(r.u8());
    return r.ok();
}

void ResultBody::Write(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(status));
    w.str(busid);
    w.str(message);
}
bool ResultBody::Read(ByteReader& r, ResultBody& out) {
    out.status = static_cast<Status>(r.u8());
    out.busid = r.str();
    out.message = r.str();
    return r.ok();
}

static void WriteDevice(ByteWriter& w, const DeviceEntry& d) {
    w.str(d.busid);
    w.str(d.instanceId);
    w.str(d.name);
    w.str(d.manufacturer);
    w.u16(d.vid);
    w.u16(d.pid);
    w.u8(d.state);
    w.u8(d.shared);
    w.u8(d.attached);
    w.u8(0);
}

static bool ReadDevice(ByteReader& r, DeviceEntry& d) {
    d.busid = r.str();
    d.instanceId = r.str();
    d.name = r.str();
    d.manufacturer = r.str();
    d.vid = r.u16();
    d.pid = r.u16();
    d.state = r.u8();
    d.shared = r.u8();
    d.attached = r.u8();
    (void)r.u8();
    return r.ok();
}

void DeviceListBody::Write(ByteWriter& w) const {
    w.u16(static_cast<uint16_t>(devices.size() > 512 ? 512 : devices.size()));
    for (size_t i = 0; i < devices.size() && i < 512; ++i) WriteDevice(w, devices[i]);
}
bool DeviceListBody::Read(ByteReader& r, DeviceListBody& out) {
    out.devices.clear();
    uint16_t n = r.u16();
    if (!r.ok() || n > 512) return false;
    for (uint16_t i = 0; i < n; ++i) {
        DeviceEntry d;
        if (!ReadDevice(r, d)) return false;
        out.devices.push_back(std::move(d));
    }
    return true;
}

void DeviceEventBody::Write(ByteWriter& w) const {
    w.u8(kind);
    WriteDevice(w, device);
}
bool DeviceEventBody::Read(ByteReader& r, DeviceEventBody& out) {
    out.kind = r.u8();
    return ReadDevice(r, out.device);
}

void BusidBody::Write(ByteWriter& w) const { w.str(busid); }
bool BusidBody::Read(ByteReader& r, BusidBody& out) {
    out.busid = r.str();
    return r.ok();
}

void NonceBody::Write(ByteWriter& w) const { w.u32(value); }
bool NonceBody::Read(ByteReader& r, NonceBody& out) {
    out.value = r.u32();
    return r.ok();
}

void ByeBody::Write(ByteWriter& w) const { w.str(reason); }
bool ByeBody::Read(ByteReader& r, ByeBody& out) {
    out.reason = r.str();
    return r.ok();
}

Sha256Digest ComputeClientProof(const uint8_t secret[16], uint64_t sessionId, const uint8_t* nonceC,
                                const uint8_t* nonceS, const uint8_t* certPin, size_t pinLen) {
    static const char kLabel[] = "MUB1-AUTH";
    HmacSha256Ctx ctx(secret, 16);
    ctx.Update(kLabel, sizeof(kLabel) - 1);
    uint8_t sid[8];
    for (int i = 0; i < 8; ++i) sid[i] = static_cast<uint8_t>(sessionId >> (8 * (7 - i)));
    ctx.Update(sid, sizeof(sid));
    ctx.Update(nonceC, kNonceLen);
    ctx.Update(nonceS, kNonceLen);
    if (certPin && pinLen) ctx.Update(certPin, pinLen);
    return ctx.Finish();
}

Sha256Digest ComputeServerProof(const uint8_t secret[16], uint64_t sessionId, const uint8_t* nonceC,
                                const uint8_t* nonceS) {
    static const char kLabel[] = "MUB1-OK";
    HmacSha256Ctx ctx(secret, 16);
    ctx.Update(kLabel, sizeof(kLabel) - 1);
    uint8_t sid[8];
    for (int i = 0; i < 8; ++i) sid[i] = static_cast<uint8_t>(sessionId >> (8 * (7 - i)));
    ctx.Update(sid, sizeof(sid));
    ctx.Update(nonceC, kNonceLen);
    ctx.Update(nonceS, kNonceLen);
    return ctx.Finish();
}

bool FrameParser::Feed(const uint8_t* data, size_t len, std::vector<Message>& out, std::string* error) {
    buf_.insert(buf_.end(), data, data + len);
    size_t pos = 0;
    while (buf_.size() - pos >= 3) {
        uint8_t type = buf_[pos];
        uint16_t n = static_cast<uint16_t>((buf_[pos + 1] << 8) | buf_[pos + 2]);
        if (buf_.size() - pos < static_cast<size_t>(3) + n) break;
        Message m;
        m.type = static_cast<MsgType>(type);
        m.payload.assign(buf_.begin() + pos + 3, buf_.begin() + pos + 3 + n);
        out.push_back(std::move(m));
        pos += 3 + n;
    }
    if (pos > 0) buf_.erase(buf_.begin(), buf_.begin() + pos);
    if (buf_.size() > (3 + kMaxFramePayload) * 4) {
        if (error) *error = "Frame buffer overflow";
        buf_.clear();
        return false;
    }
    return true;
}

}  // namespace mub
