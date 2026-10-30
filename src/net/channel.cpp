// Mini USB Bridge - control channel, byte pump and relay connector
// SPDX-License-Identifier: MIT
#include "channel.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "../core/logger.h"
#include "../core/strings.h"
#include "../win/tls.h"

namespace mub {
namespace {

constexpr const char* kRelayMagic = "MUBR1";

// Pump: TLS -> plain socket.
void PumpTlsToPlain(ControlChannel* ch, SOCKET plain, std::atomic<bool>* stop) {
    uint8_t buf[16384];
    while (!stop->load()) {
        int n = ch->Tls().Read(buf, sizeof(buf));
        if (n == -2) continue;  // receive timeout, retry
        if (n <= 0) break;
        if (!SendAll(plain, buf, static_cast<size_t>(n))) break;
    }
    stop->store(true);
    if (plain != INVALID_SOCKET) shutdown(plain, SD_BOTH);
}

// Pump: plain socket -> TLS.
void PumpPlainToTls(ControlChannel* ch, SOCKET plain, std::atomic<bool>* stop) {
    uint8_t buf[16384];
    while (!stop->load()) {
        int n = RecvSome(plain, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            int code = WSAGetLastError();
            if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) continue;
            break;
        }
        if (!ch->Tls().Write(buf, static_cast<size_t>(n))) break;
    }
    stop->store(true);
}

}  // namespace

bool ControlChannel::StartServer(SOCKET raw, const TlsIdentity& identity, std::string* err) {
    return tls_.ServerHandshake(raw, identity, err);
}

bool ControlChannel::StartClient(SOCKET raw, const uint8_t pin[10], std::string* err) {
    return tls_.ClientHandshake(raw, pin, err);
}

bool ControlChannel::Send(MsgType type, const ByteWriter& body) { return SendPayload(type, body.buf); }

bool ControlChannel::SendEmpty(MsgType type) {
    std::vector<uint8_t> payload;
    return SendPayload(type, payload);
}

bool ControlChannel::SendPayload(MsgType type, const std::vector<uint8_t>& payload) {
    Message m;
    m.type = type;
    m.payload = payload;
    std::vector<uint8_t> wire;
    m.Encode(wire);
    if (!tls_.Write(wire.data(), wire.size())) {
        closed_ = true;
        error_ = "TLS write failed";
        return false;
    }
    return true;
}

bool ControlChannel::Receive(Message& out, int timeoutMs) {
    timedOut_ = false;
    if (pendingIndex_ < pending_.size()) {
        out = pending_[pendingIndex_++];
        return true;
    }
    pending_.clear();
    pendingIndex_ = 0;
    if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), timeoutMs);
    for (;;) {
        if (!pending_.empty()) {
            out = pending_[0];
            pendingIndex_ = 1;
            return true;
        }
        uint8_t buf[16384];
        int n = tls_.Read(buf, sizeof(buf));
        if (n == -2) {
            timedOut_ = true;
            if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), 0);
            return false;
        }
        if (n <= 0) {
            closed_ = true;
            error_ = n == 0 ? "peer closed the connection" : "TLS read failed";
            if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), 0);
            return false;
        }
        std::string perr;
        if (!parser_.Feed(buf, static_cast<size_t>(n), pending_, &perr)) {
            closed_ = true;
            error_ = perr;
            if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), 0);
            return false;
        }
    }
}

bool ControlChannel::ReadExact(void* buf, size_t n, int timeoutMs) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), timeoutMs);
    while (got < n) {
        int r = tls_.Read(p + got, static_cast<int>(n - got));
        if (r == -2) continue;
        if (r <= 0) {
            if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), 0);
            timedOut_ = r == -2;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    if (timeoutMs > 0) SetRecvTimeout(tls_.Socket(), 0);
    return true;
}

void ControlChannel::Close() {
    if (!released_) tls_.Close();
    released_ = true;
}

void PipeTlsToSocket(ControlChannel& channel, SOCKET plain, std::atomic<bool>& stop) {
    // A short receive timeout keeps the TLS->socket pump responsive to 'stop',
    // otherwise it could block in recv() after the other direction is finished.
    if (channel.Tls().Socket() != INVALID_SOCKET) SetRecvTimeout(channel.Tls().Socket(), 500);
    std::thread up(PumpTlsToPlain, &channel, plain, &stop);
    std::thread down(PumpPlainToTls, &channel, plain, &stop);
    if (up.joinable()) up.join();
    stop.store(true);
    if (plain != INVALID_SOCKET) shutdown(plain, SD_BOTH);
    if (down.joinable()) down.join();
}

void PipeSockets(SOCKET a, SOCKET b) {
    std::atomic<bool> stop{false};
    auto pump = [&stop](SOCKET from, SOCKET to) {
        uint8_t buf[32768];
        while (!stop.load()) {
            int n = RecvSome(from, buf, sizeof(buf));
            if (n <= 0) {
                int code = WSAGetLastError();
                if (n < 0 && (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK)) continue;
                break;
            }
            if (!SendAll(to, buf, static_cast<size_t>(n))) break;
        }
        stop.store(true);
        shutdown(from, SD_BOTH);
        shutdown(to, SD_BOTH);
    };
    std::thread t1(pump, a, b);
    std::thread t2(pump, b, a);
    t1.join();
    t2.join();
}

SOCKET ConnectRelay(const std::string& relayHost, uint16_t relayPort, uint8_t role, uint64_t sessionId,
                    const std::string& authTag, int timeoutMs, std::string* err) {
    SOCKET s = TcpConnect(relayHost, relayPort, timeoutMs, err);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    uint8_t hello[5 + 1 + 8 + 16];
    std::memcpy(hello, kRelayMagic, 5);
    hello[5] = role;
    for (int i = 0; i < 8; ++i) hello[6 + i] = static_cast<uint8_t>(sessionId >> (8 * (7 - i)));
    if (authTag.size() < 16) {
        if (err) *err = "Relay auth tag is missing";
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    std::memcpy(hello + 14, authTag.data(), 16);
    if (!SendAll(s, hello, sizeof(hello))) {
        if (err) *err = "Relay handshake write failed";
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    SetRecvTimeout(s, timeoutMs > 0 ? timeoutMs : 30000);
    uint8_t status = 0xFF;
    int n = RecvSome(s, &status, 1);
    SetRecvTimeout(s, 0);
    if (n != 1) {
        if (err) *err = "Relay did not answer (is the relay running and reachable?)";
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    if (status == 0x01) {
        if (err) *err = "Relay rejected the session credentials";
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    if (status == 0x02) {
        if (err) *err = "Relay timed out waiting for the other side";
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    if (status != 0x00) {
        if (err) *err = "Relay returned status " + std::to_string(status);
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    if (err) err->clear();
    LogI("relay", std::string("Joined relay session ") + (role == 1 ? "(client)" : "(server)"));
    return s;
}

bool CanConnect(const std::string& host, uint16_t port, int timeoutMs) {
    SOCKET s = TcpConnect(host, port, timeoutMs, nullptr);
    if (s == INVALID_SOCKET) return false;
    CloseSocket(s);
    return true;
}

}  // namespace mub
