// Mini USB Bridge - Windows runtime compatibility layer
// SPDX-License-Identifier: MIT
#include "wincompat.h"

#if defined(_WIN32)

#include <wincrypt.h>
#include <ws2tcpip.h>

#include <mutex>

#include "../core/logger.h"
#include "../core/settings.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace mub {
namespace {

// RTL_OSVERSIONINFOW without pulling in winternl.h (which the XP toolset's SDK
// does not always provide): the layout is fixed and documented.
struct OsVersionInfoW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // GetVersionExW is deprecated but still the XP fallback
#endif

struct State {
    bool initialised = false;
    OsProfile profile;
    OsCapabilities caps;
};

State& GetState() {
    static State state;
    return state;
}

std::string Narrow(const wchar_t* text) {
    if (!text || !*text) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], need, nullptr, nullptr);
    return out;
}

// RtlGetVersion tells the truth on Windows 8.1+ where GetVersionExW lies
// (it reports 6.2 for both 8.1 and 10 unless the manifest says otherwise).
bool ReadRtlVersion(OsVersionInfoW& info) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return false;
    using Fn = LONG(WINAPI*)(OsVersionInfoW*);
    Fn rtlGetVersion = reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) return false;
    info = OsVersionInfoW{};
    info.dwOSVersionInfoSize = sizeof(info);
    return rtlGetVersion(&info) == 0;
}

WORD QueryProductType() {
    OSVERSIONINFOEXW ex{};
    ex.dwOSVersionInfoSize = sizeof(ex);
    WORD productType = 0;
    if (GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&ex))) productType = ex.wProductType;
    return productType;
}

bool ReadLegacyVersion(OsVersionInfoW& info) {
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (!GetVersionExW(&v)) return false;
    info = OsVersionInfoW{};
    info.dwOSVersionInfoSize = sizeof(info);
    info.dwMajorVersion = v.dwMajorVersion;
    info.dwMinorVersion = v.dwMinorVersion;
    info.dwBuildNumber = v.dwBuildNumber;
    info.dwPlatformId = v.dwPlatformId;
    for (int i = 0; i < 128 && v.szCSDVersion[i]; ++i) info.szCSDVersion[i] = v.szCSDVersion[i];
    return true;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

std::string ArchName(WORD nativeArch) {
    switch (nativeArch) {
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
#ifdef PROCESSOR_ARCHITECTURE_AMD64
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
#endif
#ifdef PROCESSOR_ARCHITECTURE_ARM64
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
#endif
#ifdef PROCESSOR_ARCHITECTURE_ARM
        case PROCESSOR_ARCHITECTURE_ARM: return "arm";
#endif
        case PROCESSOR_ARCHITECTURE_IA64: return "x64";
        default: return "unknown";
    }
}

std::string DetectArch(bool* wow64) {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    std::string arch = ArchName(si.wProcessorArchitecture);
    if (wow64) {
        *wow64 = false;
        using Fn = BOOL(WINAPI*)(HANDLE, PBOOL);
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        Fn isWow64 = kernel32 ? reinterpret_cast<Fn>(GetProcAddress(kernel32, "IsWow64Process")) : nullptr;
        if (isWow64) {
            BOOL value = FALSE;
            if (isWow64(GetCurrentProcess(), &value) && value) *wow64 = true;
        }
    }
    return arch;
}

}  // namespace

void InitOsSupport() {
    State& state = GetState();
    bool wow64 = false;
    std::string arch = DetectArch(&wow64);

    OsVersionInfoW info{};
    WORD productType = QueryProductType();
    if (!ReadRtlVersion(info) && !ReadLegacyVersion(info)) {
        info = OsVersionInfoW{};
    }
    if (info.dwOSVersionInfoSize == 0) {
        // Both paths failed: report an unknown OS instead of pretending.
        info.dwOSVersionInfoSize = sizeof(info);
    }

    OsVersion version;
    version.major = static_cast<int>(info.dwMajorVersion);
    version.minor = static_cast<int>(info.dwMinorVersion);
    version.build = static_cast<int>(info.dwBuildNumber);

    const bool serverSku = productType == 3 /*VER_NT_SERVER*/ || productType == 2 /*VER_NT_DOMAIN_CONTROLLER*/;

    state.profile = ClassifyOs(version, arch, serverSku);
    state.profile.wow64 = wow64;
    state.profile.servicePack = Narrow(info.szCSDVersion);
    state.profile.description = DescribeOs(state.profile);
    state.caps = CapabilitiesFor(state.profile, GetSettings().allowLegacyTls);
    state.initialised = true;

    LogI("os", "Detected " + state.profile.description);
    for (const std::string& line : StartupDiagnostics(state.profile, state.caps, GetSettings().allowLegacyTls)) {
        LogI("os", line);
    }
    if (!state.caps.serverBackend || !state.caps.clientBackend) {
        LogW("os", "USB redirection is restricted on this Windows version: " + state.caps.limitation);
    }
}

const OsProfile& OsInfo() {
    State& state = GetState();
    if (!state.initialised) InitOsSupport();
    return state.profile;
}

const OsCapabilities& OsCaps() {
    State& state = GetState();
    if (!state.initialised) InitOsSupport();
    return state.caps;
}

void RefreshOsSupport() {
    State& state = GetState();
    if (!state.initialised) {
        InitOsSupport();
        return;
    }
    state.caps = CapabilitiesFor(state.profile, GetSettings().allowLegacyTls);
    LogI("os", "TLS policy now: " + state.caps.tlsNote);
}

// ---------------------------------------------------------------------------
// CSPRNG: BCryptGenRandom (Vista+) or CryptGenRandom (Windows 2000/XP)
// ---------------------------------------------------------------------------
// Small "run exactly once" gate built on std::mutex. std::call_once would pull
// in InitOnceExecuteOnce, which Windows XP does not export (Vista+ only) and
// that would make the XP binary unloadable.
struct OnceGate {
    template <typename Fn>
    void Run(Fn fn) {
        std::lock_guard<std::mutex> lock(mu);
        if (done) return;
        fn();
        done = true;
    }
    std::mutex mu;
    bool done = false;
};

namespace {

using BCryptGenRandomFn = LONG(WINAPI*)(void*, unsigned char*, unsigned long, unsigned long);

OnceGate g_bcryptGate;
BCryptGenRandomFn g_bcryptGenRandom = nullptr;

BCryptGenRandomFn ResolveBCrypt() {
    g_bcryptGate.Run([]() {
        HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");  // does not exist on Windows XP
        if (bcrypt) {
            g_bcryptGenRandom = reinterpret_cast<BCryptGenRandomFn>(GetProcAddress(bcrypt, "BCryptGenRandom"));
        }
    });
    return g_bcryptGenRandom;
}

OnceGate g_provGate;
HCRYPTPROV g_prov = 0;
bool g_provOk = false;

bool LegacyRandomBytes(void* out, size_t len) {
    g_provGate.Run([]() {
        if (CryptAcquireContextW(&g_prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            g_provOk = true;
        } else if (CryptAcquireContextW(&g_prov, nullptr, MS_ENHANCED_PROV_W, PROV_RSA_FULL,
                                        CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            g_provOk = true;
        }
    });
    if (!g_provOk) return false;
    return CryptGenRandom(g_prov, static_cast<DWORD>(len), static_cast<BYTE*>(out)) != FALSE;
}

}  // namespace

bool MubRandomBytes(void* out, size_t len) {
    if (len == 0) return true;
    if (BCryptGenRandomFn fn = ResolveBCrypt()) {
        // BCRYPT_USE_SYSTEM_PREFERRED_RNG == 0x00000002
        if (fn(nullptr, static_cast<unsigned char*>(out), static_cast<unsigned long>(len), 0x00000002u) == 0) {
            return true;
        }
    }
    return LegacyRandomBytes(out, len);
}

// ---------------------------------------------------------------------------
// inet_pton / inet_ntop
// ---------------------------------------------------------------------------
namespace {

using InetPtonFn = int(WINAPI*)(INT, PCSTR, PVOID);
using InetNtopFn = PCSTR(WINAPI*)(INT, const void*, PSTR, size_t);

OnceGate g_ptonGate;
InetPtonFn g_inetPton = nullptr;

InetPtonFn ResolveInetPton() {
    g_ptonGate.Run([]() {
        HMODULE ws2 = LoadLibraryW(L"ws2_32.dll");
        if (ws2) g_inetPton = reinterpret_cast<InetPtonFn>(GetProcAddress(ws2, "inet_pton"));
    });
    return g_inetPton;
}

OnceGate g_ntopGate;
InetNtopFn g_inetNtop = nullptr;

InetNtopFn ResolveInetNtop() {
    g_ntopGate.Run([]() {
        HMODULE ws2 = LoadLibraryW(L"ws2_32.dll");
        if (ws2) g_inetNtop = reinterpret_cast<InetNtopFn>(GetProcAddress(ws2, "inet_ntop"));
    });
    return g_inetNtop;
}

}  // namespace

int MubInetPton(int af, const char* src, void* dst) {
    if (!src || !dst) return -1;
    if (InetPtonFn fn = ResolveInetPton()) return fn(af, src, dst);
    if (af != AF_INET) return -1;
    // XP fallback: inet_addr understands the same dotted-quad syntax.
    unsigned long addr = inet_addr(src);
    if (addr == INADDR_NONE) {
        // "255.255.255.255" is a valid address that inet_addr also reports as
        // INADDR_NONE, so confirm through WSAStringToAddress before failing.
        sockaddr_in sa{};
        int len = sizeof(sa);
        if (WSAStringToAddressA(const_cast<char*>(src), AF_INET, nullptr, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
            return -1;
        }
        addr = sa.sin_addr.s_addr;
    }
    *static_cast<unsigned long*>(dst) = addr;
    return 1;
}

const char* MubInetNtop(int af, const void* src, char* dst, size_t dstSize) {
    if (!src || !dst || dstSize == 0) return nullptr;
    if (InetNtopFn fn = ResolveInetNtop()) return fn(af, src, dst, dstSize);
    if (af != AF_INET) return nullptr;
    // XP fallback: inet_ntoa returns a per-thread static buffer, so copy it out.
    struct in_addr a;
    a.s_addr = *static_cast<const unsigned long*>(src);
    const char* text = inet_ntoa(a);
    if (!text) return nullptr;
    size_t i = 0;
    for (; text[i] && i + 1 < dstSize; ++i) dst[i] = text[i];
    dst[i] = '\0';
    return dst;
}

// ---------------------------------------------------------------------------
// DPI
// ---------------------------------------------------------------------------
UINT MubGetDpiForWindow(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    Fn getDpi = user32 ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
    if (getDpi) {
        UINT dpi = getDpi(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = hwnd ? GetDC(hwnd) : GetDC(nullptr);
    UINT dpi = 96;
    if (dc) {
        int pixels = GetDeviceCaps(dc, LOGPIXELSX);
        if (pixels > 0) dpi = static_cast<UINT>(pixels);
        ReleaseDC(hwnd, dc);
    }
    return dpi;
}

void MubEnableDpiAwareness() {
    // Per-monitor v2 (Windows 10 1703+) -> per-monitor (8.1) -> system aware (Vista+).
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx && setCtx(reinterpret_cast<HANDLE>(-4))) return;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

        using SetAwarenessFn = BOOL(WINAPI*)(int);
        auto setAwareness = reinterpret_cast<SetAwarenessFn>(GetProcAddress(user32, "SetProcessDpiAwareness"));
        if (setAwareness && SUCCEEDED(static_cast<HRESULT>(setAwareness(2)))) return;

        using SetLegacyFn = BOOL(WINAPI*)();
        auto setLegacy = reinterpret_cast<SetLegacyFn>(GetProcAddress(user32, "SetProcessDPIAware"));
        if (setLegacy && setLegacy()) return;
    }
    // Windows XP: nothing to do, the UI is drawn at 96 dpi.
}

}  // namespace mub

#endif  // _WIN32
