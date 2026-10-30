// Mini USB Bridge - Windows runtime compatibility layer
// SPDX-License-Identifier: MIT
//
// The app targets Windows XP SP3 and everything after it (Vista, 7, 8, 8.1,
// 10, 11, Server 2003-2022). Windows XP lacks several APIs the modern toolchain
// advertises at compile time (inet_pton/inet_ntop, BCryptGenRandom,
// GetDpiForWindow, SetProcessDPIAware, GetTickCount64, ...). Everything of that
// kind is funnelled through this file, resolved with GetProcAddress at runtime,
// and falls back to the XP-era API when the newer one is missing.
//
// This is the *only* file allowed to use post-XP APIs directly; ci/check_legacy_compat.py
// enforces that rule so a future change cannot silently break the XP build.
#pragma once

#if defined(_WIN32)
// Only windows.h is needed here (HWND/UINT). Translation units that also use
// Winsock include <winsock2.h> first themselves; pulling it in here would break
// files that include <windows.h> first.
#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/os_support.h"

namespace mub {

// Missing from the Windows SDKs that still ship the XP toolset.
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Detects the operating system once (RtlGetVersion, never the lying GetVersion
// unless that fails), derives the capability verdict and logs both.
void InitOsSupport();
const OsProfile& OsInfo();
const OsCapabilities& OsCaps();
// Re-reads Settings::allowLegacyTls and recomputes the TLS part of the verdict.
void RefreshOsSupport();

#if defined(_WIN32)

// CSPRNG: BCryptGenRandom (Vista+) with a CryptGenRandom (XP) fallback.
bool MubRandomBytes(void* out, size_t len);

// inet_pton / inet_ntop (Vista+ in ws2_32) with an inet_addr / WSAStringToAddress
// fallback that behaves identically for IPv4 (the only family this app uses).
int MubInetPton(int af, const char* src, void* dst);
const char* MubInetNtop(int af, const void* src, char* dst, size_t dstSize);

// GetDpiForWindow (Windows 10 1607+) with a GDI fallback (96 dpi on XP/Vista/7/8).
UINT MubGetDpiForWindow(HWND hwnd);
// Per-monitor v2 -> per-monitor -> system aware, whichever this Windows has.
void MubEnableDpiAwareness();

#endif  // _WIN32

}  // namespace mub
