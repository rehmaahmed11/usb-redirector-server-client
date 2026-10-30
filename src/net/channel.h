// Mini USB Bridge - TLS control channel + byte pump + relay connector
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../core/protocol.h"
#include "../win/net.h"
#include "../win/tls.h"

namespace mub {

class TlsIdentity;

// Framed, authenticated control channel over TLS.
class ControlChannel {
public:
    bool StartServer(SOCKET raw, const TlsIdentity& identity, std::string* err);
    bool StartClient(SOCKET raw, const uint8_t pin[10], std::string* err);

    bool Send(MsgType type, const ByteWriter& body);
    bool SendEmpty(MsgType type);
    bool SendPayload(MsgType type, const std::vector<uint8_t>& payload);

    // Waits up to timeoutMs for one frame. Returns false on timeout/close/error.
    bool Receive(Message& out, int timeoutMs);
    bool TimedOut() const { return timedOut_; }
    bool Closed() const { return closed_; }
    std::string LastError() const { return error_; }

    // Reads exactly n bytes (used for the USB/IP IMPORT header).
    bool ReadExact(void* buf, size_t n, int timeoutMs);

    void ReleaseRawSocketForPump() { released_ = true; }
    void Close();

    TlsChannel& Tls() { return tls_; }

private:
    TlsChannel tls_;
    FrameParser parser_;
    std::vector<Message> pending_;
    size_t pendingIndex_ = 0;
    bool timedOut_ = false;
    bool closed_ = false;
    bool released_ = false;
    std::string error_;
};

// Bidirectionally splices a TLS channel with a plain socket until either side
// closes. Blocks until both directions are finished.
void PipeTlsToSocket(ControlChannel& channel, SOCKET plain, std::atomic<bool>& stop);

// Byte pump between two plain sockets (used by the relay).
void PipeSockets(SOCKET a, SOCKET b);

// Connects to the relay and waits for the peer to be joined.
// role: 1 = client, 2 = server.
SOCKET ConnectRelay(const std::string& relayHost, uint16_t relayPort, uint8_t role, uint64_t sessionId,
                    const std::string& authTag, int timeoutMs, std::string* err);

// Loopback TCP connectivity test.
bool CanConnect(const std::string& host, uint16_t port, int timeoutMs);

}  // namespace mub
