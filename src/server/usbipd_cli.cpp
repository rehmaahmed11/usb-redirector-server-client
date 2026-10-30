// Mini USB Bridge - usbipd-win backend wrapper implementation
// SPDX-License-Identifier: MIT
#include "usbipd_cli.h"

#include <algorithm>
#include <cstdio>

#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../win/net.h"
#include "../win/process.h"

namespace mub {
namespace {

constexpr uint16_t kUsbipdPort = 3240;

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Splits a line on runs of 2+ spaces (usbipd list columns).
std::vector<std::string> SplitColumns(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    int spaces = 0;
    for (char c : line) {
        if (c == ' ') {
            ++spaces;
            continue;
        }
        if (spaces >= 2 && !cur.empty()) {
            cols.push_back(Trim(cur));
            cur.clear();
        } else if (spaces > 0 && !cur.empty()) {
            cur.append(static_cast<size_t>(spaces), ' ');
        }
        spaces = 0;
        cur.push_back(c);
    }
    if (!Trim(cur).empty()) cols.push_back(Trim(cur));
    return cols;
}

std::string ExtractJsonString(const std::string& line, const std::string& key) {
    size_t k = line.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    size_t colon = line.find(':', k);
    if (colon == std::string::npos) return {};
    size_t q1 = line.find('"', colon);
    if (q1 == std::string::npos) return {};
    size_t q2 = q1 + 1;
    std::string out;
    for (; q2 < line.size(); ++q2) {
        char c = line[q2];
        if (c == '\\' && q2 + 1 < line.size()) {
            char n = line[q2 + 1];
            if (n == '\\' || n == '"') {
                out.push_back(n);
                ++q2;
                continue;
            }
            if (n == 'n') {
                out.push_back('\n');
                ++q2;
                continue;
            }
            out.push_back(n);
            ++q2;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

long long ExtractJsonInt(const std::string& line, const std::string& key, bool* found) {
    if (found) *found = false;
    size_t k = line.find("\"" + key + "\"");
    if (k == std::string::npos) return 0;
    size_t colon = line.find(':', k);
    if (colon == std::string::npos) return 0;
    size_t i = colon + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] < '0' || line[i] > '9') return 0;
    if (found) *found = true;
    return std::strtoll(line.c_str() + i, nullptr, 10);
}

}  // namespace

UsbipdCli& UsbipdCli::Instance() {
    static UsbipdCli inst;
    return inst;
}

std::string UsbipdCli::ResolvePath() {
    const Settings& s = GetSettings();
    if (!path_.empty() && FileExists(path_)) return path_;
    if (!s.usbipdPath.empty() && FileExists(s.usbipdPath)) {
        path_ = s.usbipdPath;
        return path_;
    }
    // PATH lookup
    char found[MAX_PATH] = {};
    if (SearchPathA(nullptr, "usbipd.exe", nullptr, MAX_PATH, found, nullptr)) {
        path_ = found;
        return path_;
    }
    const char* candidates[] = {
        "C:\\Program Files\\usbipd-win\\usbipd.exe",
        "C:\\Program Files (x86)\\usbipd-win\\usbipd.exe",
    };
    for (const char* c : candidates) {
        if (FileExists(c)) {
            path_ = c;
            return path_;
        }
    }
    return {};
}

UsbipdInfo UsbipdCli::Probe(bool forceRefresh) {
    uint64_t now = NowUnix();
    if (!forceRefresh && probedAt_ != 0 && now - probedAt_ < 5) return cached_;

    UsbipdInfo info;
    info.path = ResolvePath();
    info.installed = !info.path.empty();
    probedAt_ = now;

    if (!info.installed) {
        info.detail = "usbipd.exe was not found. Install usbipd-win (server backend) to share USB devices.";
        cached_ = info;
        return info;
    }

    ProcessResult version = RunProcess(Widen(Quote(info.path) + " --version"), 8000);
    info.version = Trim(version.out.empty() ? version.err : version.out);

    // Service state
    ProcessResult svc = RunProcess(Widen("sc query usbipd"), 8000);
    std::string svcText = ToUpperAscii(svc.out);
    info.serviceRunning = svcText.find("RUNNING") != std::string::npos;

    info.loopbackReachable = PingLoopback(700);
    if (info.serviceRunning && info.loopbackReachable) {
        info.detail = "usbipd service is running on 127.0.0.1:3240";
    } else if (info.serviceRunning) {
        info.detail = "usbipd service is running but the local endpoint is not answering yet";
    } else {
        info.detail = "usbipd is installed but the usbipd service is not running";
    }
    cached_ = info;
    return info;
}

bool UsbipdCli::PingLoopback(int timeoutMs) {
    bool ok = false;
#if defined(_WIN32)
    SOCKET s = TcpConnect("127.0.0.1", kUsbipdPort, timeoutMs, nullptr);
    if (s != INVALID_SOCKET) {
        ok = true;
        CloseSocket(s);
    }
#else
    (void)timeoutMs;
#endif
    return ok;
}

bool UsbipdCli::ReadState(std::map<std::string, std::string>& instanceToBusid, std::string* err) {
    instanceToBusid.clear();
    UsbipdInfo info = Probe();
    if (!info.installed) {
        if (err) *err = info.detail;
        return false;
    }
    ProcessResult res = RunProcess(Widen(Quote(info.path) + " state"), 12000);
    if (!res.launched || res.timedOut || Trim(res.out).empty()) {
        if (err) *err = "usbipd state produced no output: " + res.Combined();
        return false;
    }
    // WriteIndented JSON: every property sits on its own line, so a line scan
    // is both simple and robust. BusId is either "1-2" or an object {Bus,Port}.
    std::string currentInstance;
    int pendingBus = -1;
    size_t pos = 0;
    const std::string& text = res.out;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        if (line.find("\"InstanceId\"") != std::string::npos) {
            currentInstance = ExtractJsonString(line, "InstanceId");
            pendingBus = -1;
            continue;
        }
        if (line.find("\"BusId\"") != std::string::npos && !currentInstance.empty()) {
            std::string direct = ExtractJsonString(line, "BusId");
            if (!direct.empty() && IsValidBusId(direct)) {
                instanceToBusid[currentInstance] = direct;
                currentInstance.clear();
            } else if (line.find('{') != std::string::npos) {
                bool found = false;
                long long bus = ExtractJsonInt(line, "Bus", &found);
                if (found) pendingBus = static_cast<int>(bus);
            }
            continue;
        }
        if (pendingBus >= 0 && currentInstance.size() && line.find("\"Port\"") != std::string::npos) {
            bool found = false;
            long long port = ExtractJsonInt(line, "Port", &found);
            if (found) {
                instanceToBusid[currentInstance] = std::to_string(pendingBus) + "-" + std::to_string(port);
                currentInstance.clear();
                pendingBus = -1;
            }
        }
    }
    if (err) err->clear();
    LogD("usbipd", "state -> " + std::to_string(instanceToBusid.size()) + " device(s)");
    return !instanceToBusid.empty() || res.exitCode == 0;
}

bool UsbipdCli::ReadList(std::vector<ListRow>& rows, std::string* err) {
    rows.clear();
    UsbipdInfo info = Probe();
    if (!info.installed) {
        if (err) *err = info.detail;
        return false;
    }
    ProcessResult res = RunProcess(Widen(Quote(info.path) + " list"), 12000);
    if (!res.launched || res.timedOut) {
        if (err) *err = "usbipd list failed: " + res.Combined();
        return false;
    }
    size_t pos = 0;
    while (pos <= res.out.size()) {
        size_t nl = res.out.find('\n', pos);
        std::string line = Trim(res.out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? res.out.size() + 1 : nl + 1;
        if (line.empty() || StartsWithNoCase(line, "BUSID") || StartsWithNoCase(line, "Persisted") ||
            StartsWithNoCase(line, "Connected"))
            continue;
        std::vector<std::string> cols = SplitColumns(line);
        if (cols.size() < 3) continue;
        ListRow row;
        row.busid = cols[0];
        if (!IsValidBusId(row.busid)) continue;
        row.vidpid = cols[1];
        if (cols.size() >= 4) {
            row.state = cols.back();
            std::string desc;
            for (size_t i = 2; i + 1 < cols.size(); ++i) {
                if (!desc.empty()) desc += " ";
                desc += cols[i];
            }
            row.description = desc;
        } else {
            row.description = cols[2];
        }
        rows.push_back(row);
    }
    if (rows.empty() && !Trim(res.out).empty()) {
        if (err) *err = "usbipd list output could not be parsed";
        return false;
    }
    if (err) err->clear();
    return true;
}

bool UsbipdCli::Bind(const std::string& busid, std::string& message, bool* needsElevation) {
    if (needsElevation) *needsElevation = false;
    UsbipdInfo info = Probe();
    if (!info.installed) {
        message = info.detail;
        return false;
    }
    if (!IsValidBusId(busid)) {
        message = "Invalid bus id '" + busid + "'";
        return false;
    }
    std::wstring cmd = Widen(Quote(info.path) + " bind --busid=" + busid);
    ProcessResult res = RunProcess(cmd, 20000);
    if (res.Ok()) {
        message = "Shared " + busid;
        Probe(true);
        return true;
    }
    std::string combined = Trim(res.Combined());
    message = combined.empty() ? "usbipd bind failed (exit " + std::to_string(res.exitCode) + ")" : combined;
    std::string lower = ToUpperAscii(combined);
    if (lower.find("ADMINISTRATOR") != std::string::npos || lower.find("ELEVAT") != std::string::npos ||
        lower.find("ACCESS IS DENIED") != std::string::npos) {
        if (needsElevation) *needsElevation = true;
    }
    LogW("usbipd", "bind " + busid + " failed: " + message);
    return false;
}

bool UsbipdCli::Unbind(const std::string& busid, std::string& message) {
    UsbipdInfo info = Probe();
    if (!info.installed) {
        message = info.detail;
        return false;
    }
    ProcessResult res = RunProcess(Widen(Quote(info.path) + " unbind --busid=" + busid), 20000);
    message = res.Ok() ? ("Stopped sharing " + busid) : Trim(res.Combined());
    if (message.empty()) message = "usbipd unbind failed (exit " + std::to_string(res.exitCode) + ")";
    Probe(true);
    return res.Ok();
}

bool UsbipdCli::UnbindAll(std::string& message) {
    UsbipdInfo info = Probe();
    if (!info.installed) {
        message = info.detail;
        return false;
    }
    ProcessResult res = RunProcess(Widen(Quote(info.path) + " unbind --all"), 25000);
    message = res.Ok() ? "Stopped sharing all devices" : Trim(res.Combined());
    Probe(true);
    return res.Ok();
}

bool UsbipdCli::EnsureService(std::string& message) {
    UsbipdInfo info = Probe(true);
    if (!info.installed) {
        message = info.detail;
        return false;
    }
    if (info.serviceRunning && info.loopbackReachable) {
        message = "usbipd service is running";
        return true;
    }
    ProcessResult res = RunProcess(Widen("net start usbipd"), 20000);
    Probe(true);
    UsbipdInfo after = cached_;
    if (after.serviceRunning) {
        message = "usbipd service started";
        return true;
    }
    message = Trim(res.Combined());
    if (message.empty()) message = "The usbipd service could not be started (administrator rights required)";
    return false;
}

}  // namespace mub
