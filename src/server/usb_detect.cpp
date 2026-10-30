// Mini USB Bridge - USB device enumeration
// SPDX-License-Identifier: MIT
#include "usb_detect.h"

#if defined(_WIN32)

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <usbiodef.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cstdio>

#include "../core/logger.h"
#include "../core/strings.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace mub {
namespace {

std::string WideToUtf8Trim(const wchar_t* s, DWORD chars) {
    if (!s || chars == 0) return {};
    size_t n = 0;
    while (n < chars && s[n] != L'\0') ++n;
    return WideToUtf8(std::wstring(s, n));
}

std::string GetStringProperty(HDEVINFO info, SP_DEVINFO_DATA& did, DWORD property) {
    DWORD type = 0;
    DWORD needed = 0;
    SetupDiGetDeviceRegistryPropertyW(info, &did, property, &type, nullptr, 0, &needed);
    if (needed == 0 || needed > 64 * 1024) return {};
    std::vector<BYTE> buf(needed + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(info, &did, property, &type, buf.data(), needed, nullptr)) return {};
    return WideToUtf8Trim(reinterpret_cast<const wchar_t*>(buf.data()), needed / sizeof(wchar_t));
}

std::vector<std::string> GetMultiStringProperty(HDEVINFO info, SP_DEVINFO_DATA& did, DWORD property) {
    std::vector<std::string> out;
    DWORD type = 0;
    DWORD needed = 0;
    SetupDiGetDeviceRegistryPropertyW(info, &did, property, &type, nullptr, 0, &needed);
    if (needed == 0 || needed > 256 * 1024) return out;
    std::vector<BYTE> buf(needed + sizeof(wchar_t) * 2);
    if (!SetupDiGetDeviceRegistryPropertyW(info, &did, property, &type, buf.data(), needed, nullptr)) return out;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
    size_t total = needed / sizeof(wchar_t);
    size_t i = 0;
    while (i < total && p[i]) {
        size_t start = i;
        while (i < total && p[i]) ++i;
        out.push_back(WideToUtf8(std::wstring(p + start, p + i)));
        ++i;
    }
    return out;
}

void ParseVidPid(const std::string& hardwareId, uint16_t& vid, uint16_t& pid) {
    std::string upper = ToUpperAscii(hardwareId);
    size_t vpos = upper.find("VID_");
    if (vpos != std::string::npos && vpos + 8 <= upper.size()) {
        vid = static_cast<uint16_t>(std::stoul(upper.substr(vpos + 4, 4), nullptr, 16));
    }
    size_t ppos = upper.find("PID_");
    if (ppos != std::string::npos && ppos + 8 <= upper.size()) {
        pid = static_cast<uint16_t>(std::stoul(upper.substr(ppos + 4, 4), nullptr, 16));
    }
}

bool CommaContains(const std::vector<std::string>& ids, const std::string& needle) {
    std::string n = ToUpperAscii(needle);
    for (const auto& id : ids) {
        if (ToUpperAscii(id).find(n) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

std::string UsbDevice::VidPid() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vid, pid);
    return buf;
}

std::string UsbDevice::DisplayName() const {
    if (!name.empty()) return name;
    if (!instanceId.empty()) return instanceId;
    return "Unknown USB device";
}

std::string UsbDevice::StatusText() const {
    if (!present) return "Removed";
    if (problemCode != 0) return "Problem code " + std::to_string(problemCode);
    if (attached) return "Attached to client";
    if (shared) return "Shared";
    return "Ready";
}

size_t UsbDeviceCount() {
    std::string err;
    return EnumerateUsbDevices(&err).size();
}

std::vector<UsbDevice> EnumerateUsbDevices(std::string* err) {
    std::vector<UsbDevice> devices;
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        if (err) *err = "SetupDiGetClassDevs failed (" + std::to_string(GetLastError()) + ")";
        return devices;
    }

    DWORD index = 0;
    for (;;) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, index, &interfaceData)) break;
        ++index;

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;
        std::vector<BYTE> buffer(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, needed, nullptr, &devInfo)) continue;

        UsbDevice dev;
        // Instance id
        wchar_t idBuf[512] = {};
        if (SetupDiGetDeviceInstanceIdW(info, &devInfo, idBuf, static_cast<DWORD>(std::size(idBuf)), nullptr)) {
            dev.instanceId = WideToUtf8(idBuf);
        }
        if (dev.instanceId.empty()) continue;

        dev.name = GetStringProperty(info, devInfo, SPDRP_FRIENDLYNAME);
        if (dev.name.empty()) dev.name = GetStringProperty(info, devInfo, SPDRP_DEVICEDESC);
        // SPDRP_MFC ("Manufacturer") is 0x0000000B; not all SDK headers name it.
        dev.manufacturer = GetStringProperty(info, devInfo, static_cast<DWORD>(0x0000000B));
        dev.locationInfo = GetStringProperty(info, devInfo, SPDRP_LOCATION_INFORMATION);

        std::string hardwareId = GetStringProperty(info, devInfo, SPDRP_HARDWAREID);
        ParseVidPid(hardwareId, dev.vid, dev.pid);

        std::vector<std::string> compat = GetMultiStringProperty(info, devInfo, SPDRP_COMPATIBLEIDS);
        dev.isHub = CommaContains(compat, "USB\\Class_09") ||
                    ToUpperAscii(dev.instanceId).find("ROOT_HUB") != std::string::npos;
        dev.isInput = CommaContains(compat, "Class_03&SubClass_01") || CommaContains(compat, "Class_03&SubClass_02");

        // Health / problem code (0 = working)
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devInfo.DevInst, 0) == CR_SUCCESS) {
            dev.problemCode = problem;
        }

        devices.push_back(std::move(dev));
    }
    SetupDiDestroyDeviceInfoList(info);

    std::sort(devices.begin(), devices.end(), [](const UsbDevice& a, const UsbDevice& b) {
        if (a.isHub != b.isHub) return !a.isHub;  // real devices first
        return a.DisplayName() < b.DisplayName();
    });
    if (err) err->clear();
    LogD("usb", "Enumerated " + std::to_string(devices.size()) + " USB interfaces");
    return devices;
}

bool IsDevicePresent(uint16_t vid, uint16_t pid) {
    std::string err;
    for (const auto& d : EnumerateUsbDevices(&err)) {
        if (d.vid == vid && d.pid == pid) return true;
    }
    return false;
}

}  // namespace mub

#endif  // _WIN32
