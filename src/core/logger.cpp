// Mini USB Bridge - logger implementation
// SPDX-License-Identifier: MIT
#include "logger.h"

#include <cstdio>
#include <ctime>

#include "strings.h"

namespace mub {

namespace {
constexpr size_t kRingMax = 400;
}

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

const char* Logger::LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info: return "INF";
        case LogLevel::Warn: return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}

void Logger::SetLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    path_ = path;
    if (!path_.empty()) {
        file_ = std::fopen(path_.c_str(), "ab");
    }
}

void Logger::Write(LogLevel level, const std::string& tag, const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    LogEntry e;
    e.timeUnix = NowUnix();
    e.level = level;
    e.tag = tag;
    e.text = Redact(text);
    ring_.push_back(e);
    ++seq_;
    while (ring_.size() > kRingMax) ring_.pop_front();
    if (file_ && level >= minLevel_) FlushLocked();
}

void Logger::FlushLocked() {
    if (!file_ || ring_.empty()) return;
    const LogEntry& e = ring_.back();
    std::fprintf(file_, "%s [%s] %-9s %s\n", FormatUtc(e.timeUnix).c_str(), LevelName(e.level), e.tag.c_str(),
                 e.text.c_str());
    std::fflush(file_);
}

std::vector<LogEntry> Logger::Snapshot(uint64_t sinceSeq, uint64_t& nextSeq) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LogEntry> out;
    uint64_t firstSeq = seq_ >= ring_.size() ? seq_ - ring_.size() + 1 : 1;
    for (size_t i = 0; i < ring_.size(); ++i) {
        uint64_t s = firstSeq + i;
        if (s > sinceSeq) out.push_back(ring_[i]);
    }
    nextSeq = seq_;
    return out;
}

uint64_t Logger::Sequence() const {
    std::lock_guard<std::mutex> lock(mu_);
    return seq_;
}

void LogD(const std::string& tag, const std::string& text) { Logger::Instance().Write(LogLevel::Debug, tag, text); }
void LogI(const std::string& tag, const std::string& text) { Logger::Instance().Write(LogLevel::Info, tag, text); }
void LogW(const std::string& tag, const std::string& text) { Logger::Instance().Write(LogLevel::Warn, tag, text); }
void LogE(const std::string& tag, const std::string& text) { Logger::Instance().Write(LogLevel::Error, tag, text); }

std::string Redact(const std::string& text) {
    // Pairing codes ("MUB1-...") and long hex blobs (tokens/secrets) never reach disk.
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + 4 < text.size() && StartsWithNoCase(text.substr(i, 5), "MUB1-")) {
            out += "MUB1-<redacted>";
            i += 5;
            while (i < text.size() &&
                   ((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'A' && text[i] <= 'Z') ||
                    (text[i] >= 'a' && text[i] <= 'z') || text[i] == '-'))
                ++i;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

}  // namespace mub
