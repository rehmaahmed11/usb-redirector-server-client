// Mini USB Bridge - usbip-win2 backend wrapper (client side)
// SPDX-License-Identifier: MIT
//
// usbip-win2 (BSD-2-Clause) is a separate program: it owns the UDE virtual host
// controller driver. We only invoke usbip.exe and report its real result.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

struct UsbipClientInfo {
    bool installed = false;
    std::string path;
    std::string version;
    bool supportsTcpPort = false;  // global --tcp-port option available
    bool driverPresent = false;    // UDE driver node exists
    bool osSupported = true;       // usbip-win2 can load its UDE driver on this Windows version
    bool legacyDriver = false;     // a legacy (pre-0.9.x) usbip-win VHCI driver is installed
    std::string detail;
};

class UsbipCli {
public:
    static UsbipCli& Instance();

    std::string ResolvePath();
    UsbipClientInfo Probe(bool forceRefresh = false);

    struct AttachResult {
        bool ok = false;
        int hubPort = -1;       // virtual hub port, used later for detach
        std::string message;    // raw backend message (never invented)
        bool needsElevation = false;
    };
    AttachResult Attach(const std::string& host, uint16_t tcpPort, const std::string& busid, int timeoutMs);

    struct DetachResult {
        bool ok = false;
        std::string message;
    };
    DetachResult Detach(uint16_t tcpPort, int hubPort);
    DetachResult DetachAll(uint16_t tcpPort);

    // Confirms that a USB device with this VID/PID is present locally,
    // polling for up to timeoutMs. This is the only accepted proof of attachment.
    bool ConfirmDevicePresent(uint16_t vid, uint16_t pid, int timeoutMs);

private:
    UsbipCli() = default;
    std::string path_;
    uint64_t probedAt_ = 0;
    UsbipClientInfo cached_;
};

}  // namespace mub
