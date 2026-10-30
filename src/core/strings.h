// Mini USB Bridge - string helpers (portable UTF-8 <-> UTF-16)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace mub {

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

std::string Trim(const std::string& s);
std::string ToUpperAscii(const std::string& s);
bool StartsWithNoCase(const std::string& s, const std::string& prefix);
std::string Lower(const std::string& s);

// "1-4" style USB/IP bus id sanity check (bus-port[.port]).
bool IsValidBusId(const std::string& busid);

// Formats a UTC unix timestamp as "2026-10-30 12:34:56Z".
std::string FormatUtc(uint64_t unixSeconds);
// Remaining time as "4:59" (or "--:--" when unknown/expired).
std::string FormatCountdown(uint64_t nowUnix, uint64_t expiresUnix);

std::string FormatBytes(uint64_t bytes);
std::string FormatHexBytes(const void* data, size_t len, size_t limit = 8);

uint64_t NowUnix();
uint64_t UnixAddSeconds(uint64_t base, uint64_t seconds);

}  // namespace mub
