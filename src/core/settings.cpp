// Mini USB Bridge - settings implementation
// SPDX-License-Identifier: MIT
#include "settings.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "strings.h"

namespace mub {
namespace {

std::string DataDir() {
    std::string base;
#if defined(_WIN32)
    const char* local = std::getenv("LOCALAPPDATA");
    if (!local || !*local) local = std::getenv("APPDATA");
    base = local ? local : ".";
    base += "\\MiniUsbBridge";
    for (char& c : base)
        if (c == '/') c = '\\';
#else
    const char* home = std::getenv("HOME");
    base = home ? std::string(home) + "/.config/miniusbbridge" : std::string(".miniusbbridge");
#endif
    return base;
}

bool MakeDirs(const std::string& path) {
#if defined(_WIN32)
    std::string p;
    for (size_t i = 0; i < path.size(); ++i) {
        p.push_back(path[i]);
        bool sep = (path[i] == '\\' || path[i] == '/');
        if (sep && p.size() > 3) {
            CreateDirectoryA(p.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
    return true;
#else
    std::string cmd = "mkdir -p '" + path + "' 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
#endif
}

}  // namespace

std::string Settings::Path() const {
    std::string dir = DataDir();
    MakeDirs(dir);
    return dir + (dir.find('\\') != std::string::npos ? "\\settings.ini" : "/settings.ini");
}

void Settings::SyncRaw() const {
    raw["serverPort"] = std::to_string(serverPort);
    raw["codeTtlSeconds"] = std::to_string(codeTtlSeconds);
    raw["singleUseCode"] = singleUseCode ? "1" : "0";
    raw["autoShareAll"] = autoShareAll ? "1" : "0";
    raw["autoReshareOnReenumerate"] = autoReshareOnReenumerate ? "1" : "0";
    raw["allowLegacyTls"] = allowLegacyTls ? "1" : "0";
    raw["publicHost"] = publicHost;
    raw["relayHost"] = relayHost;
    raw["relayPort"] = std::to_string(relayPort);
    raw["relayEnabled"] = relayEnabled ? "1" : "0";
    raw["tunnelPort"] = std::to_string(tunnelPort);
    raw["autoDetachOnExit"] = autoDetachOnExit ? "1" : "0";
    raw["verifyAttachWithSetupApi"] = verifyAttachWithSetupApi ? "1" : "0";
    raw["usbipdPath"] = usbipdPath;
    raw["usbipPath"] = usbipPath;
    raw["firewallHardened"] = firewallHardened ? "1" : "0";
    raw["minimizeToTray"] = minimizeToTray ? "1" : "0";
    raw["logLevel"] = std::to_string(logLevel);
    raw["logPath"] = logPath;
}

void Settings::ApplyRaw() {
    auto num = [&](const char* key, long long def, long long lo, long long hi) -> long long {
        auto it = raw.find(key);
        if (it == raw.end()) return def;
        long long v = std::strtoll(it->second.c_str(), nullptr, 10);
        if (v < lo || v > hi) return def;
        return v;
    };
    auto flag = [&](const char* key, bool def) {
        auto it = raw.find(key);
        if (it == raw.end()) return def;
        return it->second == "1" || it->second == "true" || it->second == "yes";
    };
    auto str = [&](const char* key, const std::string& def) {
        auto it = raw.find(key);
        return it == raw.end() ? def : it->second;
    };
    serverPort = static_cast<uint16_t>(num("serverPort", serverPort, 1024, 65535));
    codeTtlSeconds = static_cast<uint32_t>(num("codeTtlSeconds", codeTtlSeconds, 30, 86400));
    singleUseCode = flag("singleUseCode", singleUseCode);
    autoShareAll = flag("autoShareAll", autoShareAll);
    autoReshareOnReenumerate = flag("autoReshareOnReenumerate", autoReshareOnReenumerate);
    allowLegacyTls = flag("allowLegacyTls", allowLegacyTls);
    publicHost = Trim(str("publicHost", publicHost));
    relayHost = Trim(str("relayHost", relayHost));
    relayPort = static_cast<uint16_t>(num("relayPort", relayPort, 1, 65535));
    relayEnabled = flag("relayEnabled", relayEnabled);
    tunnelPort = static_cast<uint16_t>(num("tunnelPort", tunnelPort, 1024, 65535));
    autoDetachOnExit = flag("autoDetachOnExit", autoDetachOnExit);
    verifyAttachWithSetupApi = flag("verifyAttachWithSetupApi", verifyAttachWithSetupApi);
    usbipdPath = Trim(str("usbipdPath", usbipdPath));
    usbipPath = Trim(str("usbipPath", usbipPath));
    firewallHardened = flag("firewallHardened", firewallHardened);
    minimizeToTray = flag("minimizeToTray", minimizeToTray);
    logLevel = static_cast<int>(num("logLevel", logLevel, 0, 3));
    logPath = Trim(str("logPath", logPath));
    if (relayHost.empty()) relayEnabled = false;
}

bool Settings::Load(std::string* error) {
    SyncRaw();
    std::ifstream in(Path());
    if (!in) {
        ApplyRaw();
        return true;  // defaults are fine
    }
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        if (!key.empty()) raw[key] = value;
    }
    ApplyRaw();
    if (error) error->clear();
    return true;
}

bool Settings::Save(std::string* error) const {
    SyncRaw();
    std::string path = Path();
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            if (error) *error = "Cannot write " + path;
            return false;
        }
        out << "# Mini USB Bridge settings\n";
        for (const auto& kv : raw) out << kv.first << "=" << kv.second << "\n";
    }
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error) *error = "Cannot replace " + path;
        return false;
    }
    if (error) error->clear();
    return true;
}

Settings& GetSettings() {
    static Settings s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        s.Load();
    }
    return s;
}

}  // namespace mub
