// Mini USB Bridge - operating system support model (portable, no windows.h)
// SPDX-License-Identifier: MIT
//
// One place decides what "supported" means for every Windows version, so the
// UI, the log, the docs and the CI lint cannot drift apart:
//
//   * the app itself (Win32 GUI + TLS tunnel + relay client)  -> Windows XP SP3 and later
//   * server backend usbipd-win                               -> Windows 10 1809 (17763) x64/ARM64,
//                                                                Windows Server 2019 or later
//   * client backend usbip-win2 (UDE driver)                  -> Windows 10 1903 (18362) x64,
//                                                                Windows 11 ARM64
//
// Everything below is plain C++ (no Windows headers) so it is unit tested on
// any host -- see tests/test_core.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

// ---------------------------------------------------------------------------
// Windows versions
// ---------------------------------------------------------------------------
enum class WinOs {
    Unknown = 0,
    Win2000,
    WinXp,    // 5.1  (Windows XP 32-bit; 5.2 below covers XP x64 / Server 2003)
    WinXp64,  // 5.2  (Windows XP Professional x64 Edition = Server 2003 kernel)
    Vista,    // 6.0
    Win7,     // 6.1
    Win8,     // 6.2
    Win81,    // 6.3
    Win10,    // 10.0, build < 22000
    Win11,    // 10.0, build >= 22000
};

struct OsVersion {
    int major = 0;
    int minor = 0;
    int build = 0;
};

struct OsProfile {
    WinOs os = WinOs::Unknown;
    OsVersion version;
    std::string arch = "x64";   // "x86" | "x64" | "arm64" | "arm" | "unknown"
    bool wow64 = false;         // 32-bit image on 64-bit Windows
    bool serverSku = false;     // Windows Server / XP x64 (Server 2003 kernel)
    std::string servicePack;    // "Service Pack 1" when the OS still reports it
    std::string description;    // "Windows 7, build 7601, x64"

    bool IsLegacy() const { return os != WinOs::Unknown && os <= WinOs::Win81; }
    bool Is32Bit() const { return arch == "x86" || arch == "arm"; }
};

// Classifies a reported version. Pure function: the only input is what
// RtlGetVersion (or a test) hands over, so the mapping is reproducible.
OsProfile ClassifyOs(const OsVersion& version, const std::string& arch = "x64",
                     bool serverSku = false);

const char* WinOsName(WinOs os);   // "Windows XP", "Windows 11", ...
const char* WinOsCode(WinOs os);   // "xp", "win7", "win10", "win11", "unknown"
std::string DescribeOs(const OsProfile& profile);

// ---------------------------------------------------------------------------
// Backend facts (vendor minimums, checked 2026-10)
// ---------------------------------------------------------------------------
// usbipd-win README: "requires Microsoft Windows 10 (x64 or ARM64) / Microsoft
// Windows Server 2019, version 1809 or newer".
constexpr int kUsbipdWinMinMajor = 10;
constexpr int kUsbipdWinMinBuild = 17763;  // 1809 / Server 2019
// usbip-win2 README: "Windows 10 x64 version 1903 (OS build 18362) or later,
// Windows 11 ARM64". There is no x86 build of either backend.
constexpr int kUsbipWin2MinMajor = 10;
constexpr int kUsbipWin2MinBuild = 18362;  // 1903

// True when the reported version is at least major.build (same major line).
bool VersionAtLeast(const OsVersion& version, int major, int build);
// True for a 64-bit architecture the vendors actually ship (x64 / arm64).
bool ArchHasVendorBackend(const std::string& arch);

// ---------------------------------------------------------------------------
// TLS policy
// ---------------------------------------------------------------------------
struct TlsProfile {
    enum Bits : uint32_t {
        kNone = 0u,
        kTls10 = 1u << 0,
        kTls11 = 1u << 1,
        kTls12 = 1u << 2,
        kTls13 = 1u << 3,
    };
    uint32_t offered = 0;              // protocols this OS can offer (informational + used by tests)
    bool useSystemDefault = false;      // let SChannel use the OS/registry protocol policy
    bool strongCrypto = true;           // SCH_USE_STRONG_CRYPTO (no RC4/DES/NULL)
    bool sha1Certificate = false;       // sign a *new* identity with SHA-1 (XP SChannel)
    std::string description;            // human readable, shown in the log and the UI
};

// 'allowLegacyTls' is the user setting: when false, TLS 1.0/1.1 are never
// offered, even on an old OS (which then cannot talk to Windows XP peers).
//
// On Vista and later the policy defers to the operating system (useSystemDefault)
// as long as legacy TLS is allowed, so an administrator who disabled TLS 1.0 in
// the registry keeps that decision. Windows XP has to be told explicitly, since
// its default set is not usable by every SChannel patch level.
TlsProfile TlsProfileFor(const OsProfile& profile, bool allowLegacyTls = true);

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
struct OsCapabilities {
    bool gui = true;                 // our own window can run
    bool tunnel = true;              // our TLS tunnel can run
    bool serverBackend = false;      // usbipd-win usable on this PC
    bool clientBackend = false;      // usbip-win2 usable on this PC
    bool legacyDriverPath = false;   // only a legacy/test-signed VHCI driver can work here
    std::string serverNote;           // one line, truthful, for the SERVER screen
    std::string clientNote;           // one line, truthful, for the CLIENT screen
    std::string limitation;           // empty when both backends are usable
    std::string tlsNote;
};

OsCapabilities CapabilitiesFor(const OsProfile& profile, bool allowLegacyTls = true);

// ---------------------------------------------------------------------------
// Documentation / diagnostics
// ---------------------------------------------------------------------------
// The compatibility table published in docs/COMPATIBILITY.md is generated from
// this function, so the document cannot claim something the code does not do.
std::string CompatibilityMatrixMarkdown();

// Log/startup lines: OS, TLS policy, backend verdict, limitations.
std::vector<std::string> StartupDiagnostics(const OsProfile& profile, const OsCapabilities& caps,
                                            bool allowLegacyTls = true);

// "Windows XP SP3" style short label used in error messages.
std::string RequireOsMessage(const std::string& backend, int major, int build);

}  // namespace mub
