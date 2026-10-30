// Mini USB Bridge - child process helpers (usbipd.exe / usbip.exe wrappers)
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <string>

namespace mub {

struct ProcessResult {
    bool launched = false;
    unsigned long exitCode = 0;
    std::string out;
    std::string err;
    std::string error;  // launch-level failure description
    bool timedOut = false;

    bool Ok() const { return launched && !timedOut && exitCode == 0; }
    // stdout+stderr, trimmed, for dialogs and logs.
    std::string Combined() const { return out + (err.empty() ? "" : ("\n" + err)); }
};

// Runs a command line (hidden window, no shell) and captures output.
ProcessResult RunProcess(const std::wstring& commandLine, int timeoutMs);

// Re-launches this executable elevated with the given arguments and waits.
// 'resultFile' receives the elevated operation's result (see elevated.h).
bool RunElevated(const std::wstring& exePath, const std::wstring& arguments, const std::wstring& resultFile,
                 int timeoutMs, std::string* error);

#if defined(_WIN32)
std::wstring ExePathW();
std::string ExePath();
std::string DataDir();          // %LOCALAPPDATA%\MiniUsbBridge (created)
std::string LogFilePath();
bool FileExists(const std::string& path);
bool DirExists(const std::string& path);
bool IsProcessElevated();
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::string ReadTextFile(const std::string& path);
bool WriteTextFile(const std::string& path, const std::string& text);
std::string TempFilePath(const std::string& prefix);
#endif

}  // namespace mub
