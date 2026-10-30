// Mini USB Bridge - logger (ring buffer for the UI + rotating file sink)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mub {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct LogEntry {
    uint64_t timeUnix = 0;
    LogLevel level = LogLevel::Info;
    std::string tag;
    std::string text;
};

class Logger {
public:
    static Logger& Instance();

    void SetLogFile(const std::string& path);
    void SetMinLevel(LogLevel level) { minLevel_ = level; }
    void SetUiEnabled(bool on) { uiEnabled_ = on; }

    void Write(LogLevel level, const std::string& tag, const std::string& text);

    // Newest entries after 'sinceSeq'; returns the next sequence number to poll with.
    std::vector<LogEntry> Snapshot(uint64_t sinceSeq, uint64_t& nextSeq) const;
    uint64_t Sequence() const;

    static const char* LevelName(LogLevel l);

private:
    Logger() = default;
    void FlushLocked();

    mutable std::mutex mu_;
    std::deque<LogEntry> ring_;
    uint64_t seq_ = 0;
    std::string path_;
    FILE* file_ = nullptr;
    LogLevel minLevel_ = LogLevel::Info;
    bool uiEnabled_ = true;
};

void LogD(const std::string& tag, const std::string& text);
void LogI(const std::string& tag, const std::string& text);
void LogW(const std::string& tag, const std::string& text);
void LogE(const std::string& tag, const std::string& text);

// Removes pairing secrets and tokens from text before it is written anywhere.
std::string Redact(const std::string& text);

}  // namespace mub
