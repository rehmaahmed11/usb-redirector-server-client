// Mini USB Bridge - Winsock helpers
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// Host-side stub types so the header can be parsed by tooling on Linux.
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

bool NetInit();
void NetShutdown();

SOCKET TcpListen(uint16_t port, bool loopbackOnly, std::string* err);
SOCKET TcpAccept(SOCKET listener, std::string* peerAddress, int timeoutMs);
SOCKET TcpConnect(const std::string& host, uint16_t port, int timeoutMs, std::string* err);

bool SetRecvTimeout(SOCKET s, int ms);
bool SetSendTimeout(SOCKET s, int ms);
bool SetNoDelay(SOCKET s);
bool EnableKeepAlive(SOCKET s, int idleMs, int intervalMs);

int RecvSome(SOCKET s, void* buf, int len);       // >0 bytes, 0 = orderly close, -1 = error
bool SendAll(SOCKET s, const void* buf, size_t len);
bool IsLoopbackPeer(SOCKET s);
std::string PeerAddress(SOCKET s);

void CloseSocket(SOCKET& s);
std::string LastSocketError(int code);

// Best-effort LAN IPv4 of this machine ("" when offline).
std::string LocalIPv4();
std::string LocalHostName();
bool IsPrivateIPv4(const std::string& ip);

}  // namespace mub
