// Mini USB Bridge - string helpers
// SPDX-License-Identifier: MIT
#include "strings.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace mub {

std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            i += 4;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        if (cp <= 0x10FFFF) {
            if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            } else {
                out.push_back(static_cast<wchar_t>(cp));
            }
        }
    }
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            uint32_t lo = s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    const char* ws = " \t\r\n";
    while (b < e && std::strchr(ws, s[b])) ++b;
    while (e > b && std::strchr(ws, s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string ToUpperAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return r;
}

std::string Lower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

bool StartsWithNoCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool IsValidBusId(const std::string& busid) {
    if (busid.empty() || busid.size() > 32) return false;
    bool digits = false;
    bool dash = false;
    for (char c : busid) {
        if (c >= '0' && c <= '9') {
            digits = true;
        } else if (c == '-') {
            if (!digits || dash) return false;
            dash = true;
            digits = false;
        } else if (c == '.') {
            if (!digits) return false;
            digits = false;
        } else {
            return false;
        }
    }
    return dash && digits;
}

std::string FormatUtc(uint64_t unixSeconds) {
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tmv {};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::string FormatCountdown(uint64_t nowUnix, uint64_t expiresUnix) {
    if (expiresUnix == 0) return "--:--";
    if (nowUnix >= expiresUnix) return "expired";
    uint64_t left = expiresUnix - nowUnix;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu", static_cast<unsigned long long>(left / 60),
                  static_cast<unsigned long long>(left % 60));
    return buf;
}

std::string FormatBytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f GiB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string FormatHexBytes(const void* data, size_t len, size_t limit) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s;
    char buf[4];
    for (size_t i = 0; i < len && i < limit; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        s += buf;
        if (i + 1 < len && i + 1 < limit) s += ' ';
    }
    if (len > limit) s += " ...";
    return s;
}

void SplitHostPort(const std::string& input, std::string* host, uint16_t* port) {
    if (host) host->clear();
    if (port) *port = 0;
    std::string text = Trim(input);
    if (text.empty()) return;

    // Drop a scheme prefix ("tcp://", "tls://", "http://", ...) - tunnel
    // dashboards hand out addresses in that form and users paste them as-is.
    size_t scheme = text.find("://");
    if (scheme != std::string::npos && scheme < 12) text = text.substr(scheme + 3);
    // Drop any path part ("host:port/") and trailing slashes.
    size_t slash = text.find('/');
    if (slash != std::string::npos) text = text.substr(0, slash);
    text = Trim(text);
    if (text.empty()) return;

    std::string hostPart = text;
    uint16_t portValue = 0;

    if (!text.empty() && text[0] == '[') {
        // [IPv6]:port
        size_t close = text.find(']');
        if (close != std::string::npos) {
            hostPart = text.substr(1, close - 1);
            if (close + 1 < text.size() && text[close + 1] == ':') portValue = 0;
            if (close + 1 < text.size() && text[close + 1] == ':') {
                const std::string tail = text.substr(close + 2);
                unsigned long v = 0;
                bool digits = !tail.empty();
                for (char c : tail) {
                    if (c < '0' || c > '9') { digits = false; break; }
                    v = v * 10 + static_cast<unsigned long>(c - '0');
                    if (v > 65535) { digits = false; break; }
                }
                portValue = digits ? static_cast<uint16_t>(v) : 0;
            }
            if (host) *host = hostPart;
            if (port) *port = portValue;
            return;
        }
    }

    size_t colon = text.rfind(':');
    if (colon != std::string::npos && text.find(':') == colon) {  // exactly one ':'
        const std::string tail = text.substr(colon + 1);
        unsigned long v = 0;
        bool digits = !tail.empty() && tail.size() <= 5;
        for (char c : tail) {
            if (c < '0' || c > '9') { digits = false; break; }
            v = v * 10 + static_cast<unsigned long>(c - '0');
        }
        if (digits && v >= 1 && v <= 65535) {
            portValue = static_cast<uint16_t>(v);
            hostPart = text.substr(0, colon);
        }
    }

    hostPart = Trim(hostPart);
    if (host && !hostPart.empty()) *host = hostPart;
    if (port) *port = portValue;
}

uint64_t NowUnix() { return static_cast<uint64_t>(::time(nullptr)); }

uint64_t UnixAddSeconds(uint64_t base, uint64_t seconds) { return base + seconds; }

}  // namespace mub
