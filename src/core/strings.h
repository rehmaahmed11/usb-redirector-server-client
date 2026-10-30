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

// Splits a user-entered address into host + optional port. Accepts
//   "host" / "host:port" / "1.2.3.4:5555" / "[::1]:5555" / "tcp://host:port"
// and tolerates spaces, a trailing '/' and an upper-case scheme.
// port is set to 0 when the text has no (valid) port, in which case the caller
// keeps its own default. Scheme prefixes are dropped, so a URL pasted from a
// tunnel dashboard ("tcp://foo.example.com:12345") works unchanged.
void SplitHostPort(const std::string& input, std::string* host, uint16_t* port);

// Formats a UTC unix timestamp as "2026-10-30 12:34:56Z".
std::string FormatUtc(uint64_t unixSeconds);
// Remaining time as "4:59" (or "--:--" when unknown/expired).
std::string FormatCountdown(uint64_t nowUnix, uint64_t expiresUnix);

std::string FormatBytes(uint64_t bytes);
std::string FormatHexBytes(const void* data, size_t len, size_t limit = 8);

uint64_t NowUnix();
uint64_t UnixAddSeconds(uint64_t base, uint64_t seconds);

}  // namespace mub
