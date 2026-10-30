// Mini USB Bridge - Winsock helpers implementation
// SPDX-License-Identifier: MIT
#include "net.h"

#if defined(_WIN32)

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mstcpip.h>  // tcp_keepalive / SIO_KEEPALIVE_VALS: MSVC needs this header explicitly

#include <cstring>
#include <mutex>

#include "../core/logger.h"
#include "wincompat.h"

#pragma comment(lib, "ws2_32.lib")

namespace mub {
namespace {
// std::mutex instead of std::call_once: call_once imports InitOnceExecuteOnce,
// which Windows XP does not have.
std::mutex g_wsaMutex;
int g_wsaResult = -1;
bool g_wsaDone = false;
}  // namespace

bool NetInit() {
    std::lock_guard<std::mutex> lock(g_wsaMutex);
    if (!g_wsaDone) {
        WSADATA wsa;
        g_wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsaDone = true;
    }
    return g_wsaResult == 0;
}

void NetShutdown() { WSACleanup(); }

std::string LastSocketError(int code) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0,
        nullptr);
    std::string msg = "socket error " + std::to_string(code);
    if (n && buf) {
        std::string s(buf, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        msg += " (" + s + ")";
    }
    if (buf) LocalFree(buf);
    return msg;
}

void CloseSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

bool SetRecvTimeout(SOCKET s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t)) == 0;
}

bool SetSendTimeout(SOCKET s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    return setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t)) == 0;
}

bool SetNoDelay(SOCKET s) {
    BOOL on = TRUE;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on)) == 0;
}

bool EnableKeepAlive(SOCKET s, int idleMs, int intervalMs) {
    BOOL on = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on)) != 0)
        return false;
    tcp_keepalive ka{};
    ka.onoff = 1;
    ka.keepalivetime = static_cast<ULONG>(idleMs);
    ka.keepaliveinterval = static_cast<ULONG>(intervalMs);
    DWORD returned = 0;
    return WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &returned, nullptr, nullptr) == 0;
}

SOCKET TcpListen(uint16_t port, bool loopbackOnly, std::string* err) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (err) *err = LastSocketError(WSAGetLastError());
        return INVALID_SOCKET;
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = loopbackOnly ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int code = WSAGetLastError();
        if (err) *err = "Cannot bind port " + std::to_string(port) + ": " + LastSocketError(code);
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 16) != 0) {
        if (err) *err = "listen failed: " + LastSocketError(WSAGetLastError());
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET TcpAccept(SOCKET listener, std::string* peerAddress, int timeoutMs) {
    if (timeoutMs > 0) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener, &rd);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int r = select(0, &rd, nullptr, nullptr, &tv);
        if (r <= 0) return INVALID_SOCKET;
    }
    sockaddr_in addr{};
    int len = sizeof(addr);
    SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    if (s != INVALID_SOCKET && peerAddress) {
        char buf[64] = {};
        MubInetNtop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        *peerAddress = buf;
    }
    return s;
}

SOCKET TcpConnect(const std::string& host, uint16_t port, int timeoutMs, std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        if (err) *err = "Cannot resolve '" + host + "'";
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        if (err) *err = LastSocketError(WSAGetLastError());
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0) {
        int code = WSAGetLastError();
        if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
            if (err) *err = "Connect to " + host + ":" + portStr + " failed: " + LastSocketError(code);
            CloseSocket(s);
            return INVALID_SOCKET;
        }
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, nullptr, &wr, &ex, &tv);
        if (sel <= 0) {
            if (err) *err = "Connect to " + host + ":" + portStr + " timed out";
            CloseSocket(s);
            return INVALID_SOCKET;
        }
        int soErr = 0;
        int soLen = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
        if (soErr != 0) {
            if (err) *err = "Connect to " + host + ":" + portStr + " failed: " + LastSocketError(soErr);
            CloseSocket(s);
            return INVALID_SOCKET;
        }
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    if (err) err->clear();
    return s;
}

int RecvSome(SOCKET s, void* buf, int len) { return recv(s, static_cast<char*>(buf), len, 0); }

bool SendAll(SOCKET s, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string PeerAddress(SOCKET s) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
    char buf[64] = {};
    MubInetNtop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return buf;
}

bool IsLoopbackPeer(SOCKET s) {
    std::string ip = PeerAddress(s);
    return ip.rfind("127.", 0) == 0;
}

bool IsPrivateIPv4(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 169 && b == 254) return true;
    if (a == 127) return true;
    return false;
}

std::string LocalIPv4() {
    // Connect a UDP socket to a public address (no packets are sent) to learn the
    // interface Windows would use for outbound traffic.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    MubInetPton(AF_INET, "1.1.1.1", &dst.sin_addr);
    std::string result;
    if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
        sockaddr_in src{};
        int len = sizeof(src);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&src), &len) == 0) {
            char buf[64] = {};
            MubInetNtop(AF_INET, &src.sin_addr, buf, sizeof(buf));
            result = buf;
        }
    }
    closesocket(s);
    if (result.rfind("127.", 0) == 0) return {};
    return result;
}

std::string LocalHostName() {
    char buf[256] = {};
    DWORD len = sizeof(buf);
    if (!GetComputerNameA(buf, &len)) return "Windows PC";
    return buf;
}

}  // namespace mub

#endif  // _WIN32
