// Mini USB Bridge - relay server (NAT traversal for two machines without a
// reachable inbound port). Small on purpose: it authenticates both sides with
// the pairing session secret, then splices bytes. It cannot read the TLS stream
// that flows through it, and it does not terminate TLS.
//
// Build (Windows): cmake --build . --target mubb-relay
// Build (Linux):   g++ -std=c++17 -O2 -pthread relay/relay.cpp -o mubb-relay
// Run:             mubb-relay [--port 4443] [--verbose]
// SPDX-License-Identifier: MIT
//
// Wire protocol (see docs/ARCHITECTURE.md):
//   both sides send: "MUBR1" | role(1) | sessionId(8, BE) | authTag(16)
//   relay answers:   0x00 paired (then raw bytes both ways), 0x01 bad tag, 0x02 timeout/busy
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SD_BOTH SHUT_RDWR
inline int closesocket(SOCKET s) { return ::close(s); }
inline int WSAGetLastError() { return errno; }
inline int WSAStartup(int, void*) { return 0; }
inline void WSACleanup() {}
inline void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#define MAKEWORD(a, b) ((a) | ((b) << 8))
#endif

namespace {

constexpr uint16_t kDefaultPort = 4443;
constexpr size_t kHelloLen = 5 + 1 + 8 + 16;
constexpr int kJoinTimeoutMs = 90000;
constexpr int kMaxSessions = 512;
constexpr int kMaxPerSender = 20;

std::atomic<bool> g_verbose{false};
std::mutex g_logMu;

void Log(const char* fmt, ...) {
    if (!g_verbose.load()) return;
    std::lock_guard<std::mutex> lock(g_logMu);
    time_t now = ::time(nullptr);
    struct tm tmv {};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::printf("%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

struct Session {
    uint64_t id = 0;
    std::string tag;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    std::string clientIp, serverIp;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::atomic<bool> paired{false};
};

std::mutex g_sessionsMu;
std::map<uint64_t, std::shared_ptr<Session>> g_sessions;
std::condition_variable g_cond;

void SetRecvTimeout(SOCKET s, int ms) {
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void SetSendTimeout(SOCKET s, int ms) {
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void SetNoDelay(SOCKET s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

bool SendAllSafe(SOCKET s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(s, p + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void Pump(SOCKET from, SOCKET to) {
    char buf[32768];
    for (;;) {
        int n = ::recv(from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!SendAllSafe(to, buf, static_cast<size_t>(n))) break;
    }
    ::shutdown(from, SD_BOTH);
    ::shutdown(to, SD_BOTH);
}

std::string IpOf(SOCKET s) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
#if defined(_WIN32) && defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
    // Windows XP has no inet_ntop; inet_ntoa is good enough for a log line.
    const char* text = inet_ntoa(addr.sin_addr);
    return text ? text : "?";
#else
    char buf[64] = {};
    // xp-ok: only compiled for Vista and later (the #if above excludes XP)
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return buf;
#endif
}

void CleanupExpired() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_sessionsMu);
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        bool stale = now - it->second->created > std::chrono::seconds(300);
        bool dead = it->second->client == INVALID_SOCKET && it->second->server == INVALID_SOCKET;
        if (stale || dead) {
            Log("session %016llx removed", static_cast<unsigned long long>(it->first));
            it = g_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

int g_activeSessions = 0;

void HandlePeer(SOCKET sock) {
    SetRecvTimeout(sock, kJoinTimeoutMs);
    SetNoDelay(sock);

    uint8_t hello[kHelloLen];
    size_t got = 0;
    while (got < kHelloLen) {
        int n = ::recv(sock, reinterpret_cast<char*>(hello) + got, static_cast<int>(kHelloLen - got), 0);
        if (n <= 0) {
            closesocket(sock);
            return;
        }
        got += static_cast<size_t>(n);
    }
    if (std::memcmp(hello, "MUBR1", 5) != 0) {
        uint8_t bad = 0x01;
        SendAllSafe(sock, &bad, 1);
        closesocket(sock);
        return;
    }
    uint8_t role = hello[5];
    uint64_t sessionId = 0;
    for (int i = 0; i < 8; ++i) sessionId = (sessionId << 8) | hello[6 + i];
    std::string tag(reinterpret_cast<char*>(hello) + 14, 16);
    std::string ip = IpOf(sock);

    if ((role != 1 && role != 2) || sessionId == 0) {
        uint8_t bad = 0x01;
        SendAllSafe(sock, &bad, 1);
        closesocket(sock);
        return;
    }

    if (g_activeSessions >= kMaxSessions) {
        uint8_t busy = 0x02;
        SendAllSafe(sock, &busy, 1);
        closesocket(sock);
        Log("rejected %s: session limit reached", ip.c_str());
        return;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(g_sessionsMu);
        auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            session = std::make_shared<Session>();
            session->id = sessionId;
            session->tag = tag;
            g_sessions[sessionId] = session;
            Log("session %016llx created by %s (role %d)", static_cast<unsigned long long>(sessionId), ip.c_str(),
                role);
        } else {
            session = it->second;
            // The auth tag binds the session id to the pairing secret, so a third
            // party that guesses the id cannot join or hijack it.
            if (session->tag != tag) {
                uint8_t bad = 0x01;
                SendAllSafe(sock, &bad, 1);
                closesocket(sock);
                Log("rejected %s: auth tag mismatch for session %016llx", ip.c_str(),
                    static_cast<unsigned long long>(sessionId));
                return;
            }
            SOCKET& slot = (role == 1) ? session->client : session->server;
            if (slot != INVALID_SOCKET) {
                uint8_t busy = 0x02;
                SendAllSafe(sock, &busy, 1);
                closesocket(sock);
                Log("rejected %s: role %d already connected", ip.c_str(), role);
                return;
            }
            slot = sock;
            if (role == 1)
                session->clientIp = ip;
            else
                session->serverIp = ip;
        }
        if (role == 1 && session->client == INVALID_SOCKET) {
            session->client = sock;
            session->clientIp = ip;
        } else if (role == 2 && session->server == INVALID_SOCKET) {
            session->server = sock;
            session->serverIp = ip;
        }
        if (session->client != INVALID_SOCKET && session->server != INVALID_SOCKET) {
            session->paired.store(true);
            g_cond.notify_all();  // wake the peer that is already waiting
        }
    }

    // Wait for the other side.
    {
        std::unique_lock<std::mutex> lock(g_sessionsMu);
        bool ok = g_cond.wait_for(lock, std::chrono::milliseconds(kJoinTimeoutMs),
                                  [&]() { return session->paired.load(); });
        if (!ok) {
            uint8_t timeoutStatus = 0x02;
            SendAllSafe(sock, &timeoutStatus, 1);
            SOCKET& slot = (role == 1) ? session->client : session->server;
            if (slot == sock) slot = INVALID_SOCKET;
            closesocket(sock);
            Log("session %016llx timed out waiting for the peer", static_cast<unsigned long long>(sessionId));
            return;
        }
    }

    // Both ends are ready: confirm, then splice.
    uint8_t ok = 0x00;
    if (!SendAllSafe(sock, &ok, 1)) {
        closesocket(sock);
        return;
    }
    SOCKET other = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(g_sessionsMu);
        other = (role == 1) ? session->server : session->client;
    }
    if (other == INVALID_SOCKET) {
        closesocket(sock);
        return;
    }
    SetRecvTimeout(sock, 0);
    SetRecvTimeout(other, 0);
    SetNoDelay(other);
    ++g_activeSessions;
    Log("session %016llx paired: %s <-> %s", static_cast<unsigned long long>(sessionId),
        session->clientIp.c_str(), session->serverIp.c_str());

    std::thread a(Pump, sock, other);
    std::thread b(Pump, other, sock);
    a.join();
    b.join();
    --g_activeSessions;

    {
        std::lock_guard<std::mutex> lock(g_sessionsMu);
        SOCKET& slot = (role == 1) ? session->client : session->server;
        if (slot == sock) slot = INVALID_SOCKET;
        SOCKET& otherSlot = (role == 1) ? session->server : session->client;
        if (otherSlot == other) otherSlot = INVALID_SOCKET;
    }
    closesocket(sock);
    Log("session %016llx closed (%s)", static_cast<unsigned long long>(sessionId), ip.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--verbose" || arg == "-v") g_verbose.store(true);
        else if (arg == "--help" || arg == "-h") {
            std::printf("mubb-relay [--port 4443] [--verbose]\n"
                        "Relay for Mini USB Bridge. It only splices encrypted TLS bytes; "
                        "it never sees pairing secrets in clear text.\n");
            return 0;
        }
    }

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        return 1;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        std::fprintf(stderr, "cannot listen on port %u: %d\n", port, WSAGetLastError());
        return 1;
    }
    std::printf("mubb-relay listening on 0.0.0.0:%u (verbose: %s)\n", port, g_verbose.load() ? "yes" : "no");
    std::fflush(stdout);
    std::thread janitor([]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            CleanupExpired();
        }
    });
    janitor.detach();

    for (;;) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        SOCKET sock = accept(listener, reinterpret_cast<sockaddr*>(&peer), &len);
        if (sock == INVALID_SOCKET) continue;
        std::thread(HandlePeer, sock).detach();
    }
    return 0;
}
