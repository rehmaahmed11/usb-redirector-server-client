// Mini USB Bridge - operating system support model
// SPDX-License-Identifier: MIT
#include "os_support.h"

#include <cstdio>

namespace mub {
namespace {

std::string BuildText(const OsVersion& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d", v.major, v.minor, v.build);
    return buf;
}

std::string MarketingName(const OsVersion& v) {
    // Windows 10/11 share 10.0; the build number is the only discriminator.
    if (v.major == 10 && v.minor == 0) {
        if (v.build >= 22000) return "Windows 11";
        if (v.build >= 19041) return "Windows 10 2004-22H2";
        if (v.build >= 18362) return "Windows 10 1903-1909";
        if (v.build >= 17763) return "Windows 10 1809";
        if (v.build >= 17134) return "Windows 10 1803";
        if (v.build >= 16299) return "Windows 10 1709";
        if (v.build >= 15063) return "Windows 10 1703";
        if (v.build >= 14393) return "Windows 10 1607";
        if (v.build >= 10586) return "Windows 10 1511";
        return "Windows 10 1507";
    }
    switch (v.major * 100 + v.minor) {
        case 500: return "Windows 2000";
        case 501: return "Windows XP";
        case 502: return "Windows XP x64 / Server 2003";
        case 600: return "Windows Vista";
        case 601: return "Windows 7";
        case 602: return "Windows 8";
        case 603: return "Windows 8.1";
        default: return "unknown Windows version";
    }
}

}  // namespace

bool VersionAtLeast(const OsVersion& version, int major, int build) {
    if (version.major != major) return version.major > major;
    return version.build >= build;
}

bool ArchHasVendorBackend(const std::string& arch) { return arch == "x64" || arch == "arm64"; }

OsProfile ClassifyOs(const OsVersion& version, const std::string& arch, bool serverSku) {
    OsProfile p;
    p.version = version;
    p.arch = arch.empty() ? "unknown" : arch;
    p.serverSku = serverSku;

    switch (version.major * 100 + version.minor) {
        case 500: p.os = WinOs::Win2000; break;
        case 501: p.os = WinOs::WinXp; break;
        case 502:
            p.os = WinOs::WinXp64;
            p.serverSku = true;  // XP x64 is the Server 2003 kernel
            break;
        case 600: p.os = WinOs::Vista; break;
        case 601: p.os = WinOs::Win7; break;
        case 602: p.os = WinOs::Win8; break;
        case 603: p.os = WinOs::Win81; break;
        default:
            if (version.major == 10 && version.minor == 0) {
                p.os = version.build >= 22000 ? WinOs::Win11 : WinOs::Win10;
            } else if (version.major > 10) {
                p.os = WinOs::Win11;  // future Windows keeps the 10.0 line going
            } else {
                p.os = WinOs::Unknown;
            }
            break;
    }

    p.description = DescribeOs(p);
    return p;
}

const char* WinOsName(WinOs os) {
    switch (os) {
        case WinOs::Win2000: return "Windows 2000";
        case WinOs::WinXp: return "Windows XP";
        case WinOs::WinXp64: return "Windows XP x64 / Server 2003";
        case WinOs::Vista: return "Windows Vista";
        case WinOs::Win7: return "Windows 7";
        case WinOs::Win8: return "Windows 8";
        case WinOs::Win81: return "Windows 8.1";
        case WinOs::Win10: return "Windows 10";
        case WinOs::Win11: return "Windows 11";
        default: return "unknown Windows version";
    }
}

const char* WinOsCode(WinOs os) {
    switch (os) {
        case WinOs::Win2000: return "win2000";
        case WinOs::WinXp: return "xp";
        case WinOs::WinXp64: return "xp64";
        case WinOs::Vista: return "vista";
        case WinOs::Win7: return "win7";
        case WinOs::Win8: return "win8";
        case WinOs::Win81: return "win81";
        case WinOs::Win10: return "win10";
        case WinOs::Win11: return "win11";
        default: return "unknown";
    }
}

std::string DescribeOs(const OsProfile& profile) {
    std::string name = MarketingName(profile.version);
    std::string out = name;
    if (profile.serverSku && name.rfind("Server", 0) == std::string::npos &&
        name.rfind("Windows Server", 0) == std::string::npos) {
        // XP x64 / Server 2003 already carries it in the name; a Windows Server
        // SKU running the 10.0 line is labelled explicitly by the caller.
        if (profile.os == WinOs::WinXp64) out += " (Server 2003 kernel)";
    }
    if (!profile.servicePack.empty()) out += " " + profile.servicePack;
    out += ", build " + BuildText(profile.version);
    if (!profile.arch.empty() && profile.arch != "unknown") out += ", " + profile.arch;
    if (profile.wow64) out += " (32-bit app on 64-bit Windows)";
    return out;
}

TlsProfile TlsProfileFor(const OsProfile& profile, bool allowLegacyTls) {
    TlsProfile t;
    // Every Windows version this app starts on ships SChannel with TLS 1.0
    // (Windows 2000 SP3 and later), so 1.0 is always the floor for the OS.
    const uint32_t kLegacy = TlsProfile::kTls10 | TlsProfile::kTls11;

    switch (profile.os) {
        case WinOs::Win2000:
        case WinOs::WinXp:
        case WinOs::WinXp64:
            // SChannel on 5.x speaks TLS 1.0 only (AES suites arrive with XP SP3).
            // RC4/3DES may be the only option on an unpatched box, so the strong
            // crypto flag has to stay off here.
            t.offered = TlsProfile::kTls10;
            t.useSystemDefault = false;
            t.strongCrypto = false;
            t.sha1Certificate = true;
            t.description = "TLS 1.0 (legacy SChannel, SHA-1 certificates)";
            break;
        case WinOs::Vista:
            t.offered = TlsProfile::kTls10 | TlsProfile::kTls11 | TlsProfile::kTls12;
            t.useSystemDefault = true;
            t.strongCrypto = true;
            t.sha1Certificate = false;
            t.description = "OS protocol policy (TLS 1.0-1.2 on this SChannel)";
            break;
        case WinOs::Win7:
        case WinOs::Win8:
        case WinOs::Win81:
            t.offered = TlsProfile::kTls10 | TlsProfile::kTls11 | TlsProfile::kTls12;
            t.useSystemDefault = true;
            t.strongCrypto = true;
            t.description = "OS protocol policy, TLS 1.2 preferred (TLS 1.0 stays available for XP peers)";
            break;
        case WinOs::Win10:
        case WinOs::Win11:
            // SCHANNEL_CRED cannot request TLS 1.3 (Microsoft's TLS 1.3 support
            // requires the newer SCH_CREDENTIALS structure). The handshake
            // therefore settles on TLS 1.2, which every supported peer understands.
            // TLS 1.0/1.1 stay offered so an XP/Vista peer can still connect.
            t.offered = TlsProfile::kTls12 | TlsProfile::kTls11 | TlsProfile::kTls10;
            t.useSystemDefault = true;
            t.strongCrypto = true;
            t.description = "OS protocol policy, TLS 1.2 preferred (TLS 1.0/1.1 accepted for old peers)";
            break;
        default:
            t.offered = TlsProfile::kTls12;
            t.useSystemDefault = false;
            t.strongCrypto = true;
            t.description = "TLS 1.2 (unknown OS, conservative default)";
            break;
    }

    if (!allowLegacyTls) {
        if (profile.os <= WinOs::WinXp64) {
            t.description += " - WARNING: TLS 1.0 is the only protocol this OS offers";
        } else {
            t.offered &= ~kLegacy;
            t.useSystemDefault = false;  // pin the protocol set to TLS 1.2
            t.description += " - TLS 1.0/1.1 disabled by setting";
        }
    }
    return t;
}

OsCapabilities CapabilitiesFor(const OsProfile& profile, bool allowLegacyTls) {
    OsCapabilities c;
    const OsVersion& v = profile.version;
    TlsProfile tls = TlsProfileFor(profile, allowLegacyTls);

    const bool known = profile.os != WinOs::Unknown;
    c.gui = known && profile.os >= WinOs::Win2000;
    c.tunnel = known;
    c.tlsNote = tls.description;

    const bool x64 = ArchHasVendorBackend(profile.arch);
    const bool modern = profile.os == WinOs::Win10 || profile.os == WinOs::Win11;

    c.serverBackend = modern && VersionAtLeast(v, kUsbipdWinMinMajor, kUsbipdWinMinBuild) && x64;
    c.clientBackend = modern && VersionAtLeast(v, kUsbipWin2MinMajor, kUsbipWin2MinBuild) && x64;

    if (c.serverBackend) {
        c.serverNote = "usbipd-win can run on this PC";
    } else if (!modern) {
        c.serverNote = std::string("usbipd-win needs Windows 10 1809 or later (x64/ARM64); this PC runs ") +
                       DescribeOs(profile) + " - SERVER mode cannot export USB devices here";
    } else if (!x64) {
        c.serverNote = "usbipd-win ships x64/ARM64 only; 32-bit Windows cannot run the server backend";
    } else {
        c.serverNote = "usbipd-win needs Windows 10 1809 (build 17763) or later; this PC is older";
    }

    if (c.clientBackend) {
        c.clientNote = "usbip-win2 driver can run on this PC";
    } else if (!modern) {
        c.clientNote = std::string("usbip-win2 needs Windows 10 1903 or later (x64) or Windows 11 ARM64; this PC runs ") +
                       DescribeOs(profile) + " - no supported client driver";
    } else if (!x64) {
        c.clientNote = "usbip-win2 ships x64/ARM64 only; 32-bit Windows cannot load its UDE driver";
    } else {
        c.clientNote = "usbip-win2 needs Windows 10 1903 (build 18362) or later; upgrade Windows first";
    }

    c.legacyDriverPath = !c.clientBackend;

    if (!c.serverBackend && !c.clientBackend) {
        c.limitation = std::string("App, pairing and encrypted tunnel run on ") + DescribeOs(profile) +
                       ", but USB redirection needs a backend that this Windows version cannot run: "
                       "usbipd-win (server) needs Windows 10 1809+, usbip-win2 (client driver) needs Windows 10 1903+. "
                       "On this PC only the tunnel/relay path, diagnostics and the pairing UI work.";
        if (c.legacyDriverPath) {
            c.limitation += " A legacy test-signed VHCI driver (for example cezanne/usbip-win v0.1.0) is the only "
                            "known option for the client side; it is not bundled and not supported by this project.";
        }
    } else if (!c.serverBackend) {
        c.limitation = "SERVER mode is not available on this PC (usbipd-win needs Windows 10 1809 x64/ARM64).";
    } else if (!c.clientBackend) {
        c.limitation = "CLIENT mode cannot attach devices on this PC (usbip-win2 needs Windows 10 1903 x64).";
    }

    if (!known) {
        c.limitation = "The Windows version could not be identified; the app runs in conservative mode.";
    }
    return c;
}

std::string RequireOsMessage(const std::string& backend, int major, int build) {
    std::string out = backend + " needs Windows ";
    if (major == 10 && build == kUsbipdWinMinBuild) out += "10 1809 (build 17763)";
    else if (major == 10 && build == kUsbipWin2MinBuild) out += "10 1903 (build 18362)";
    else {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d.0 build %d", major, build);
        out += buf;
    }
    out += " or later";
    return out;
}

std::string CompatibilityMatrixMarkdown() {
    struct Row {
        const char* label;
        OsVersion v;
        const char* arch;
    };
    const Row rows[] = {
        {"Windows XP SP3 (x86)", {5, 1, 2600}, "x86"},
        {"Windows XP x64 Edition / Server 2003", {5, 2, 3790}, "x64"},
        {"Windows Vista SP2", {6, 0, 6002}, "x64"},
        {"Windows 7 SP1", {6, 1, 7601}, "x64"},
        {"Windows 8", {6, 2, 9200}, "x64"},
        {"Windows 8.1", {6, 3, 9600}, "x64"},
        {"Windows 10 1507 - 1803", {10, 0, 17134}, "x64"},
        {"Windows 10 1809", {10, 0, 17763}, "x64"},
        {"Windows 10 1903 - 1909", {10, 0, 18363}, "x64"},
        {"Windows 10 2004 - 22H2", {10, 0, 19045}, "x64"},
        {"Windows 10 (32-bit x86)", {10, 0, 19045}, "x86"},
        {"Windows 11 21H2 - 25H2", {10, 0, 26100}, "x64"},
        {"Windows 11 ARM64", {10, 0, 26100}, "arm64"},
        {"Windows Server 2019", {10, 0, 17763}, "x64"},
        {"Windows Server 2022", {10, 0, 20348}, "x64"},
    };

    std::string md = "| Windows version | App (GUI + tunnel) | Server backend (usbipd-win) | Client backend (usbip-win2) | TLS used |\n";
    md += "|---|---|---|---|---|\n";
    for (const Row& r : rows) {
        OsProfile p = ClassifyOs(r.v, r.arch);
        OsCapabilities c = CapabilitiesFor(p, true);
        std::string app = c.gui && c.tunnel ? "yes" : "no";
        std::string server = c.serverBackend ? "yes" : "**no**";
        std::string client = c.clientBackend ? "yes" : "**no**";
        std::string tls;
        if (p.os <= WinOs::WinXp64) tls = "TLS 1.0";
        else if (p.os <= WinOs::Win81) tls = "TLS 1.2 (1.0-1.2 offered)";
        else tls = "TLS 1.2";
        md += "| " + std::string(r.label) + " | " + app + " | " + server + " | " + client + " | " + tls + " |\n";
    }
    return md;
}

std::vector<std::string> StartupDiagnostics(const OsProfile& profile, const OsCapabilities& caps,
                                            bool allowLegacyTls) {
    (void)allowLegacyTls;
    std::vector<std::string> out;
    out.push_back("OS: " + DescribeOs(profile));
    out.push_back("TLS: " + caps.tlsNote);
    out.push_back(std::string("Server backend (usbipd-win): ") + (caps.serverBackend ? "supported" : "NOT supported") +
                  " - " + caps.serverNote);
    out.push_back(std::string("Client backend (usbip-win2): ") + (caps.clientBackend ? "supported" : "NOT supported") +
                  " - " + caps.clientNote);
    if (!caps.limitation.empty()) out.push_back("Limitation: " + caps.limitation);
    return out;
}

}  // namespace mub
