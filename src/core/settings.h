// Mini USB Bridge - settings (INI file, no registry, portable parsing)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace mub {

struct Settings {
    // --- server ---
    uint16_t serverPort = 4437;            // direct TLS listener (TCP)
    uint32_t codeTtlSeconds = 300;         // pairing code lifetime
    bool singleUseCode = true;
    bool autoShareAll = false;             // "share all" without per-device confirmation
    bool requireApproval = true;           // always true in the UI; kept for clarity
    bool autoReshareOnReenumerate = false; // never auto-attach; re-share only when asked
    bool allowLegacyTls = true;            // offer TLS 1.0/1.1 so Windows XP/Vista peers can connect
    std::string publicHost;                // DDNS name / public IP for direct mode (optional)

    // --- relay ---
    std::string relayHost;                 // empty = relay disabled
    uint16_t relayPort = 4443;
    bool relayEnabled = false;

    // --- client ---
    uint16_t tunnelPort = 4240;            // loopback port handed to usbip.exe -t
    bool autoDetachOnExit = true;
    bool verifyAttachWithSetupApi = true;

    // --- backends ---
    std::string usbipdPath;                // empty = search PATH + default install dirs
    std::string usbipPath;

    // --- misc ---
    bool firewallHardened = false;
    bool minimizeToTray = false;
    int logLevel = 1;
    std::string logPath;

    std::string Path() const;

    bool Load(std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;

    // Raw access for the settings dialog (key/value pairs already present in the file).
    mutable std::map<std::string, std::string> raw;
    void SyncRaw() const;
    void ApplyRaw();
};

Settings& GetSettings();

}  // namespace mub
