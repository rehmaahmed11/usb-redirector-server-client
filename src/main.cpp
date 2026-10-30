// Mini USB Bridge - application entry point
// SPDX-License-Identifier: MIT
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

#include "client/client_mode.h"
#include "core/logger.h"
#include "core/protocol.h"
#include "core/settings.h"
#include "core/strings.h"
#include "server/server_mode.h"
#include "ui/main_window.h"
#include "ui/ui_event.h"
#include "win/crypto_win.h"
#include "win/elevated.h"
#include "win/net.h"
#include "win/process.h"
#include "win/tls.h"
#include "win/wincompat.h"

namespace {

// One instance per user session: the server binds a port and owns the identity
// file, so a second copy would only confuse the user.
HANDLE AcquireSingleInstance(HWND& existingWindow) {
    existingWindow = nullptr;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\MiniUsbBridge.SingleInstance");
    if (!mutex) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        existingWindow = FindWindowW(L"MiniUsbBridgeMainWindow", nullptr);
        return mutex;  // still owned, caller must exit
    }
    return mutex;
}

std::string ArgValue(const std::vector<std::string>& args, const std::string& key,
                     const std::string& fallback = {}) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) return args[i + 1];
    }
    return fallback;
}

bool HasArg(const std::vector<std::string>& args, const std::string& key) {
    for (const auto& a : args)
        if (a == key) return true;
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // Elevated helper mode: no window, no UI, single operation, then exit.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(mub::WideToUtf8(argv[i]));
    if (argv) LocalFree(argv);
    if (HasArg(args, "--op")) {
        return mub::RunElevatedOperation(args);
    }

    // Per-monitor v2 -> per-monitor -> system aware, whichever this Windows has
    // (Windows XP and Vista fall back to the GDI resolution).
    mub::MubEnableDpiAwareness();
    InitCommonControls();

    HWND existing = nullptr;
    HANDLE instanceMutex = AcquireSingleInstance(existing);
    if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    mub::Settings& settings = mub::GetSettings();
    std::string logPath = settings.logPath.empty() ? mub::LogFilePath() : settings.logPath;
    mub::Logger::Instance().SetLogFile(logPath);
    mub::Logger::Instance().SetMinLevel(static_cast<mub::LogLevel>(settings.logLevel));
    mub::LogI("app", "Mini USB Bridge starting (log: " + logPath + ")");

    // Detect the Windows version once: it decides the TLS policy and whether the
    // USB backends can run here at all (see src/core/os_support.cpp).
    mub::InitOsSupport();

    if (!mub::NetInit()) {
        MessageBoxW(nullptr, L"Winsock could not be initialized.", L"Mini USB Bridge", MB_OK | MB_ICONERROR);
        return 1;
    }

    // TLS identity: generated on first run, DPAPI-protected, reused afterwards.
    auto identity = std::make_shared<mub::TlsIdentity>();
    std::string identityError;
    std::string identityPath = mub::DataDir() + "\\identity.bin";
    bool haveIdentity = mub::LoadOrCreateIdentity(*identity, identityPath, &identityError);
    if (!haveIdentity) {
        mub::LogE("app", "TLS identity unavailable: " + identityError);
    } else {
        mub::LogI("app", "Server certificate pin " + mub::Hex(identity->pin, sizeof(identity->pin)));
    }
    mub::ServerMode::Instance().SetIdentity(identity);

    mub::MainWindow& window = mub::MainWindow::Instance();
    if (!window.Create(instance, showCommand)) {
        MessageBoxW(nullptr, L"The main window could not be created.", L"Mini USB Bridge", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!haveIdentity) {
        auto ev = mub::MakeUiEvent(mub::UiKind::Banner,
                                   "Secure identity could not be created: " + identityError, "error");
        mub::PostUi(std::move(ev));
    } else if (!mub::OsCaps().limitation.empty()) {
        // Say it once, plainly: this Windows version cannot run the USB backends.
        auto ev = mub::MakeUiEvent(mub::UiKind::Banner, mub::OsCaps().limitation, "warn");
        mub::PostUi(std::move(ev));
    }

    // Optional command line shortcuts for the installer and power users.
    if (HasArg(args, "--server")) {
        PostMessageW(window.Handle(), WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), 0);
    } else if (HasArg(args, "--client")) {
        PostMessageW(window.Handle(), WM_COMMAND, MAKEWPARAM(102, BN_CLICKED), 0);
    }
    std::string mode = ArgValue(args, "--mode");
    if (mode == "server") PostMessageW(window.Handle(), WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), 0);
    if (mode == "client") PostMessageW(window.Handle(), WM_COMMAND, MAKEWPARAM(102, BN_CLICKED), 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    mub::ServerMode::Instance().Stop();
    if (mub::ClientMode::Instance().Running()) mub::ClientMode::Instance().Disconnect("exit");
    mub::NetShutdown();
    mub::LogI("app", "Mini USB Bridge exited");
    if (instanceMutex) CloseHandle(instanceMutex);
    return 0;
}
