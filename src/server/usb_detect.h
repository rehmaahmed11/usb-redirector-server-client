// Mini USB Bridge - USB device enumeration (SetupAPI)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

struct UsbDevice {
    std::string instanceId;      // Windows device instance id
    std::string name;            // friendly name (falls back to device description)
    std::string manufacturer;
    std::string busid;           // USB/IP bus id ("1-4"), empty when unknown
    std::string locationInfo;    // e.g. "Port_#0002.Hub_#0003"
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint32_t problemCode = 0;
    bool present = true;
    bool shared = false;         // bound / exportable through usbipd
    bool attached = false;       // currently attached to a client
    bool isHub = false;
    bool isInput = false;        // HID keyboard/mouse: sharing it removes local input
    std::string backendState;    // raw state string from the backend ("Shared", ...)

    std::string VidPid() const;  // "0e8d:2000"
    std::string DisplayName() const;
    std::string StatusText() const;
};

// Enumerates real USB devices (interfaces), never empty hub ports.
std::vector<UsbDevice> EnumerateUsbDevices(std::string* err);

// Detects a device with the given VID/PID (used to confirm a client-side attach).
bool IsDevicePresent(uint16_t vid, uint16_t pid);

// Number of USB devices seen (cheap health metric for the UI).
size_t UsbDeviceCount();

}  // namespace mub
