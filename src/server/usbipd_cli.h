// Mini USB Bridge - usbipd-win backend wrapper (server side)
// SPDX-License-Identifier: MIT
//
// usbipd-win is a separate GPL-3.0 program; we only run its CLI and read its
// output. No code from it is linked into this application.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mub {

struct UsbipdInfo {
    bool installed = false;
    std::string path;
    std::string version;
    bool serviceRunning = false;
    bool loopbackReachable = false;  // 127.0.0.1:3240 accepts connections
    std::string detail;
};

class UsbipdCli {
public:
    static UsbipdCli& Instance();

    // Resolution order: explicit setting, PATH, default install locations.
    std::string ResolvePath();
    UsbipdInfo Probe(bool forceRefresh = false);

    // instanceId -> busid from `usbipd state` (JSON). Returns false when the
    // backend is missing or the JSON could not be understood.
    bool ReadState(std::map<std::string, std::string>& instanceToBusid, std::string* err);

    struct ListRow {
        std::string busid;
        std::string vidpid;
        std::string description;
        std::string state;  // "Not shared" / "Shared" / "Attached"
    };
    // Parses `usbipd list` text output (fallback / sharing state).
    bool ReadList(std::vector<ListRow>& rows, std::string* err);

    bool Bind(const std::string& busid, std::string& message, bool* needsElevation);
    bool Unbind(const std::string& busid, std::string& message);
    bool UnbindAll(std::string& message);
    bool EnsureService(std::string& message);

    // true when the local USB/IP endpoint answers (usbipd is really serving).
    bool PingLoopback(int timeoutMs);

private:
    UsbipdCli() = default;
    std::string path_;
    uint64_t probedAt_ = 0;
    UsbipdInfo cached_;
};

}  // namespace mub
