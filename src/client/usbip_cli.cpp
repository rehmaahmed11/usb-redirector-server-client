// Mini USB Bridge - usbip-win2 backend wrapper implementation
// SPDX-License-Identifier: MIT
#include "usbip_cli.h"

#include <chrono>
#include <thread>

#include "../core/logger.h"
#include "../core/os_support.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../server/usb_detect.h"
#include "../win/process.h"
#include "../win/wincompat.h"

namespace mub {
namespace {

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

int ExtractPortNumber(const std::string& text) {
    // usbip.exe prints e.g. "successfully attached to port 1" (or just "1" with --terse).
    std::string lower = Lower(text);
    size_t p = lower.find("port");
    auto scan = [&](size_t from) -> int {
        size_t i = from;
        while (i < text.size() && (text[i] == ' ' || text[i] == ':' || text[i] == '#')) ++i;
        int value = -1;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (value < 0) value = 0;
            value = value * 10 + (text[i] - '0');
            ++i;
            if (value > 255) return -1;
        }
        return value;
    };
    if (p != std::string::npos) {
        int v = scan(p + 4);
        if (v > 0) return v;
    }
    // Fall back: any standalone number in the last part of the message.
    int v = scan(0);
    return v > 0 ? v : -1;
}

// A legacy usbip-win (cezanne/ren2r line, e.g. v0.1.0) installs a WDM VHCI
// driver instead of the modern UDE driver. It still speaks USB/IP 1.1.1, so it
// can be used, but it is not signed for modern Windows and its CLI differs.
bool LegacyVhciPresent() {
    const char* files[] = {
        "C:\\Windows\\System32\\drivers\\usbip_vhci.sys",
        "C:\\Windows\\System32\\drivers\\usbip_vhci_ude.sys",
    };
    for (const char* f : files) {
        if (FileExists(f)) return true;
    }
    return false;
}

bool IsElevationError(const std::string& text) {
    std::string u = ToUpperAscii(text);
    return u.find("ADMINISTRATOR") != std::string::npos || u.find("ELEVAT") != std::string::npos ||
           u.find("ACCESS IS DENIED") != std::string::npos;
}

}  // namespace

UsbipCli& UsbipCli::Instance() {
    static UsbipCli inst;
    return inst;
}

std::string UsbipCli::ResolvePath() {
    const Settings& s = GetSettings();
    if (!path_.empty() && FileExists(path_)) return path_;
    if (!s.usbipPath.empty() && FileExists(s.usbipPath)) {
        path_ = s.usbipPath;
        return path_;
    }
    char found[MAX_PATH] = {};
    if (SearchPathA(nullptr, "usbip.exe", nullptr, MAX_PATH, found, nullptr)) {
        path_ = found;
        return path_;
    }
    const char* candidates[] = {
        "C:\\Program Files\\USBip\\usbip.exe",
        "C:\\Program Files (x86)\\USBip\\usbip.exe",
    };
    for (const char* c : candidates) {
        if (FileExists(c)) {
            path_ = c;
            return path_;
        }
    }
    return {};
}

UsbipClientInfo UsbipCli::Probe(bool forceRefresh) {
    uint64_t now = NowUnix();
    if (!forceRefresh && probedAt_ != 0 && now - probedAt_ < 5) return cached_;

    UsbipClientInfo info;
    info.path = ResolvePath();
    info.installed = !info.path.empty();
    info.osSupported = OsCaps().clientBackend;
    probedAt_ = now;
    if (!info.installed) {
        info.legacyDriver = LegacyVhciPresent();
        if (!info.osSupported && info.legacyDriver) {
            info.detail = "No usbip.exe found, but a legacy VHCI driver is installed. " + OsCaps().clientNote;
        } else if (!info.osSupported) {
            info.detail = OsCaps().clientNote;
        } else {
            info.detail = "usbip.exe was not found. Install usbip-win2 (client driver) to attach remote devices.";
        }
        cached_ = info;
        return info;
    }

    ProcessResult version = RunProcess(Widen("\"" + info.path + "\" --version"), 8000);
    info.version = Trim(version.out.empty() ? version.err : version.out);

    ProcessResult help = RunProcess(Widen("\"" + info.path + "\" --help"), 8000);
    std::string helpText = Lower(help.out + help.err);
    info.supportsTcpPort = helpText.find("tcp-port") != std::string::npos;

    // The driver node is created by the installer; the file check catches
    // legacy installs that place the driver elsewhere.
    info.legacyDriver = LegacyVhciPresent() && !DirExists("C:\\Program Files\\USBip");
    info.driverPresent = DirExists("C:\\Program Files\\USBip") || LegacyVhciPresent();
    if (!info.osSupported) {
        info.detail = info.driverPresent
                          ? "usbip.exe " + info.version + " found, but this Windows version cannot load the usbip-win2 UDE driver: " +
                                OsCaps().clientNote
                          : OsCaps().clientNote;
    } else if (!info.supportsTcpPort) {
        info.detail = "usbip.exe " + info.version +
                      " (older build: the tunnel must use TCP port 3240)";
    } else if (info.legacyDriver) {
        info.detail = "usbip.exe " + info.version + " with a legacy WDM VHCI driver (test-signed, unsupported)";
    } else {
        info.detail = "usbip.exe " + info.version + " (custom TCP port supported)";
    }
    cached_ = info;
    return info;
}

UsbipCli::AttachResult UsbipCli::Attach(const std::string& host, uint16_t tcpPort, const std::string& busid,
                                        int timeoutMs) {
    AttachResult result;
    UsbipClientInfo info = Probe();
    if (!info.installed) {
        result.message = info.detail;
        return result;
    }
    if (!IsValidBusId(busid)) {
        result.message = "Invalid bus id '" + busid + "'";
        return result;
    }

    if (!info.osSupported && !info.legacyDriver) {
        // Refuse to pretend: without a driver this Windows can load, the attach
        // cannot produce a device in Device Manager.
        result.message = info.detail;
        LogW("usbip", "attach refused on unsupported Windows version: " + info.detail);
        return result;
    }
    if (info.legacyDriver) {
        LogW("usbip", "legacy VHCI driver in use - attach is best effort and unsupported");
    }

    std::string cmd = "\"" + info.path + "\"";
    if (info.supportsTcpPort) cmd += " --tcp-port " + std::to_string(tcpPort);
    cmd += " attach -r " + host + " -b " + busid;

    ProcessResult res = RunProcess(Widen(cmd), timeoutMs);
    std::string text = Trim(res.Combined());
    // Old builds reject the global --tcp-port option; retry the plain form (the
    // tunnel is bound to 3240 in that case, see ClientMode).
    if (res.launched && res.exitCode != 0 && info.supportsTcpPort &&
        (Lower(text).find("unknown option") != std::string::npos ||
         Lower(text).find("invalid option") != std::string::npos)) {
        std::string fallback = "\"" + info.path + "\" attach -r " + host + " -b " + busid;
        LogW("usbip", "--tcp-port rejected by usbip.exe, retrying with the legacy syntax");
        res = RunProcess(Widen(fallback), timeoutMs);
        text = Trim(res.Combined());
    }
    result.message = text;
    if (!res.launched) {
        result.message = res.error;
        return result;
    }
    if (res.timedOut) {
        result.message = "usbip.exe attach timed out after " + std::to_string(timeoutMs) + " ms";
        return result;
    }
    std::string lower = Lower(text);
    bool looksOk = res.exitCode == 0 && lower.find("fail") == std::string::npos &&
                   lower.find("error") == std::string::npos;
    if (looksOk) {
        result.ok = true;
        result.hubPort = ExtractPortNumber(text);
        LogI("usbip", "attach " + busid + " -> " + text);
    } else {
        result.ok = false;
        result.needsElevation = IsElevationError(text);
        if (text.empty()) result.message = "usbip.exe attach failed (exit " + std::to_string(res.exitCode) + ")";
        LogW("usbip", "attach " + busid + " failed: " + result.message);
    }
    return result;
}

UsbipCli::DetachResult UsbipCli::Detach(uint16_t tcpPort, int hubPort) {
    DetachResult result;
    UsbipClientInfo info = Probe();
    if (!info.installed) {
        result.message = info.detail;
        return result;
    }
    if (hubPort <= 0) return DetachAll(tcpPort);
    std::string cmd = "\"" + info.path + "\"";
    if (info.supportsTcpPort) cmd += " --tcp-port " + std::to_string(tcpPort);
    cmd += " detach -p " + std::to_string(hubPort);
    ProcessResult res = RunProcess(Widen(cmd), 20000);
    result.ok = res.exitCode == 0;
    result.message = Trim(res.Combined());
    if (result.message.empty())
        result.message = result.ok ? "Detached port " + std::to_string(hubPort)
                                   : "usbip.exe detach failed (exit " + std::to_string(res.exitCode) + ")";
    return result;
}

UsbipCli::DetachResult UsbipCli::DetachAll(uint16_t tcpPort) {
    DetachResult result;
    UsbipClientInfo info = Probe();
    if (!info.installed) {
        result.message = info.detail;
        return result;
    }
    std::string cmd = "\"" + info.path + "\"";
    if (info.supportsTcpPort) cmd += " --tcp-port " + std::to_string(tcpPort);
    cmd += " detach -a";
    ProcessResult res = RunProcess(Widen(cmd), 20000);
    result.ok = res.exitCode == 0;
    result.message = Trim(res.Combined());
    if (result.message.empty())
        result.message = result.ok ? "Detached all virtual devices"
                                   : "usbip.exe detach -a failed (exit " + std::to_string(res.exitCode) + ")";
    return result;
}

bool UsbipCli::ConfirmDevicePresent(uint16_t vid, uint16_t pid, int timeoutMs) {
    int elapsed = 0;
    while (elapsed < timeoutMs) {
        if (IsDevicePresent(vid, pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        elapsed += 250;
    }
    return false;
}

}  // namespace mub
