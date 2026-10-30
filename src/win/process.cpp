// Mini USB Bridge - child process helpers
// SPDX-License-Identifier: MIT
#include "process.h"

#if defined(_WIN32)

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <thread>

#include "../core/logger.h"
#include "../core/strings.h"

#pragma comment(lib, "shell32.lib")

namespace mub {
namespace {

void DrainPipe(HANDLE pipe, std::string& sink) {
    CHAR buf[4096];
    DWORD avail = 0;
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) || read == 0) break;
        buf[read] = '\0';
        sink.append(buf, read);
    }
}

}  // namespace

std::wstring Widen(const std::string& s) { return Utf8ToWide(s); }
std::string Narrow(const std::wstring& s) { return WideToUtf8(s); }

std::wstring ExePathW() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return L"";
    return std::wstring(buf, n);
}

std::string ExePath() { return Narrow(ExePathW()); }

std::string DataDir() {
    const char* local = std::getenv("LOCALAPPDATA");
    std::string dir = local && *local ? std::string(local) + "\\MiniUsbBridge" : std::string(".\\MiniUsbBridge");
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string LogFilePath() {
    std::string dir = DataDir();
    CreateDirectoryA((dir + "\\logs").c_str(), nullptr);
    return dir + "\\logs\\miniusbbridge.log";
}

bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD len = 0;
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string TempFilePath(const std::string& prefix) {
    char buf[MAX_PATH] = {};
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    GetTempFileNameA(tmp, "mub", 0, buf);
    std::string path = buf;
    // Keep a descriptive suffix so logs are readable; CreateProcess does not care.
    std::string renamed = path + "." + prefix;
    if (MoveFileA(path.c_str(), renamed.c_str())) return renamed;
    return path;
}

ProcessResult RunProcess(const std::wstring& commandLine, int timeoutMs) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;  // CreateProcessW needs a mutable buffer
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!ok) {
        DWORD e = GetLastError();
        result.error = "CreateProcess failed (" + std::to_string(e) + ") for: " + Narrow(commandLine);
        CloseHandle(outRead);
        CloseHandle(errRead);
        return result;
    }
    result.launched = true;

    DWORD start = GetTickCount();
    for (;;) {
        DrainPipe(outRead, result.out);
        DrainPipe(errRead, result.err);
        DWORD wait = WaitForSingleObject(pi.hProcess, 25);
        if (wait == WAIT_OBJECT_0) break;
        if (timeoutMs > 0 && GetTickCount() - start > static_cast<DWORD>(timeoutMs)) {
            TerminateProcess(pi.hProcess, 1);
            result.timedOut = true;
            break;
        }
    }
    // Final drain.
    DrainPipe(outRead, result.out);
    DrainPipe(errRead, result.err);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);
    CloseHandle(errRead);

    LogD("proc", Narrow(commandLine) + " -> exit " + std::to_string(code) +
                   (result.timedOut ? " (timeout)" : ""));
    return result;
}

bool RunElevated(const std::wstring& exePath, const std::wstring& arguments, const std::wstring& resultFile,
                 int timeoutMs, std::string* error) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.c_str();
    sei.lpParameters = arguments.c_str();
    sei.nShow = SW_HIDE;

    DeleteFileW(resultFile.c_str());
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (error) {
            *error = e == ERROR_CANCELLED ? "Administrator permission was declined"
                                          : "Cannot start the elevated helper (" + std::to_string(e) + ")";
        }
        return false;
    }
    if (!sei.hProcess) {
        if (error) *error = "Elevated helper did not start";
        return false;
    }
    DWORD wait = WaitForSingleObject(sei.hProcess, timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (wait != WAIT_OBJECT_0) {
        if (error) *error = "Elevated helper timed out";
        return false;
    }
    if (code != 0) {
        if (error) *error = "Elevated helper failed (exit " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

}  // namespace mub

#endif  // _WIN32
