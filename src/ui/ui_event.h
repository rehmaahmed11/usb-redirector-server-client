// Mini USB Bridge - UI event plumbing (worker threads -> window)
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/protocol.h"

namespace mub {

constexpr UINT kUiEventMessage = WM_APP + 0x51;

enum class UiKind {
    ServerState,    // a = state code, b = human text
    ServerCode,     // a = pairing code text, n = expiry (unix)
    ServerDevices,  // devices
    ServerClient,   // a = client name, b = peer, c = path (Direct/Relay), flag = connected
    ClientState,    // a = state code, b = text
    ClientDevices,  // devices
    ClientServer,   // a = server name, b = endpoint, c = tls info
    Approval,       // approval request
    LogTick,        // a = short status line for the log strip
    Banner,         // a = text, b = severity ("info"/"warn"/"error")
};

// Posted by the server when a client asks for access; the UI fills in the
// decision and the worker thread continues.
struct ApprovalRequest {
    std::string clientName;
    std::string peer;
    std::string path;
    std::string requested;  // e.g. "2 shared devices"
    int decision = 0;       // 0 pending, 1 allow, 2 reject

    void Decide(bool allow) {
        {
            std::lock_guard<std::mutex> lock(mu);
            decision = allow ? 1 : 2;
        }
        cv.notify_all();
    }
    // Returns true when a decision was made within timeoutMs; false = timeout.
    bool Wait(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return decision != 0; });
    }

    std::mutex mu;
    std::condition_variable cv;
};

struct UiEvent {
    UiKind kind = UiKind::LogTick;
    std::string a, b, c;
    uint64_t n = 0;
    uint64_t n2 = 0;
    bool flag = false;
    std::vector<DeviceEntry> devices;
    std::shared_ptr<ApprovalRequest> approval;
};

class UiQueue {
public:
    static UiQueue& Instance() {
        static UiQueue q;
        return q;
    }
    void SetWindow(HWND hwnd) {
        std::lock_guard<std::mutex> lock(mu_);
        hwnd_ = hwnd;
    }
    void Post(std::shared_ptr<UiEvent> ev) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(std::move(ev));
        }
#if defined(_WIN32)
        HWND hwnd;
        {
            std::lock_guard<std::mutex> lock(mu_);
            hwnd = hwnd_;
        }
        if (hwnd) PostMessageW(hwnd, kUiEventMessage, 0, 0);
#endif
    }
    std::shared_ptr<UiEvent> Pop() {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.empty()) return nullptr;
        auto ev = queue_.front();
        queue_.erase(queue_.begin());
        return ev;
    }

private:
    std::mutex mu_;
    std::vector<std::shared_ptr<UiEvent>> queue_;
    HWND hwnd_ = nullptr;
};

inline void PostUi(std::shared_ptr<UiEvent> ev) { UiQueue::Instance().Post(std::move(ev)); }

inline std::shared_ptr<UiEvent> MakeUiEvent(UiKind kind, std::string a = {}, std::string b = {}) {
    auto ev = std::make_shared<UiEvent>();
    ev->kind = kind;
    ev->a = std::move(a);
    ev->b = std::move(b);
    return ev;
}

}  // namespace mub
