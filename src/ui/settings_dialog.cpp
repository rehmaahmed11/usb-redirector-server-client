// Mini USB Bridge - settings + approval dialogs
// SPDX-License-Identifier: MIT
#include "settings_dialog.h"

#if defined(_WIN32)

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../server/server_mode.h"
#include "../win/elevated.h"
#include "../win/process.h"
#include "theme.h"
#include "ui_event.h"

namespace mub {
namespace {

constexpr int kIdEditServerPort = 201;
constexpr int kIdEditTtl = 202;
constexpr int kIdCheckSingleUse = 203;
constexpr int kIdEditPublicHost = 204;
constexpr int kIdEditRelayHost = 205;
constexpr int kIdEditRelayPort = 206;
constexpr int kIdCheckRelay = 207;
constexpr int kIdEditTunnelPort = 208;
constexpr int kIdEditUsbipd = 209;
constexpr int kIdEditUsbip = 210;
constexpr int kIdCheckDetach = 211;
constexpr int kIdCheckVerify = 212;
constexpr int kIdSave = 220;
constexpr int kIdCancel = 221;
constexpr int kIdLogs = 222;
constexpr int kIdFirewall = 223;
constexpr int kIdCheckAutoShare = 224;
constexpr int kIdEditLogLevel = 225;

struct SettingsCtx {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    HFONT font = nullptr;
    bool saved = false;
    std::vector<HWND> controls;
};

std::wstring W(const std::string& s) { return Utf8ToWide(s); }
const std::wstring& W(const std::wstring& s) { return s; }
std::string N(const std::wstring& s) { return WideToUtf8(s); }

std::string GetText(HWND edit) {
    wchar_t buf[1024] = {};
    GetWindowTextW(edit, buf, 1024);
    return Trim(N(buf));
}

void AddLabel(SettingsCtx* ctx, const wchar_t* text, int x, int y, int w) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 18, ctx->hwnd, nullptr, nullptr,
                            nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->font), TRUE);
    ctx->controls.push_back(h);
}

HWND AddEdit(SettingsCtx* ctx, int id, const std::string& value, int x, int y, int w) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", W(value).c_str(),
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 24, ctx->hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->font), TRUE);
    ctx->controls.push_back(h);
    return h;
}

HWND AddCheck(SettingsCtx* ctx, int id, const wchar_t* label, bool checked, int x, int y, int w) {
    HWND h = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w,
                            20, ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->font), TRUE);
    SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    ctx->controls.push_back(h);
    return h;
}

HWND AddButton(SettingsCtx* ctx, int id, const wchar_t* label, int x, int y, int w, int h) {
    HWND b = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h,
                            ctx->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->font), TRUE);
    ctx->controls.push_back(b);
    return b;
}

bool IsChecked(HWND hwnd, int id) {
    return SendDlgItemMessageW(hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<SettingsCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ctx = static_cast<SettingsCtx*>(cs->lpCreateParams);
            ctx->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme::kText);
            SetBkColor(dc, theme::kBg);
            static HBRUSH brush = CreateSolidBrush(theme::kBg);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (!ctx) return 0;
            if (id == kIdCancel) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kIdLogs) {
                std::string dir = DataDir() + "\\logs";
                ShellExecuteA(hwnd, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (id == kIdFirewall) {
                Settings& s = GetSettings();
                bool enable = !s.firewallHardened;
                ElevatedResult er;
                if (ElevatedFirewallBlock(enable, er)) {
                    MessageBoxW(hwnd,
                                enable ? L"Inbound TCP 3240 is now blocked.\n\nThe raw USB/IP port can no longer "
                                         L"be reached from the network; only this app's encrypted channel is used."
                                       : L"The firewall block rule was removed.",
                                L"Mini USB Bridge", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, W(er.message.empty() ? er.error : er.message).c_str(), L"Firewall",
                                MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            if (id == kIdSave) {
                Settings& s = GetSettings();
                s.serverPort = static_cast<uint16_t>(std::atoi(GetText(GetDlgItem(hwnd, kIdEditServerPort)).c_str()));
                s.codeTtlSeconds = static_cast<uint32_t>(std::atoi(GetText(GetDlgItem(hwnd, kIdEditTtl)).c_str()));
                s.singleUseCode = IsChecked(hwnd, kIdCheckSingleUse);
                s.publicHost = GetText(GetDlgItem(hwnd, kIdEditPublicHost));
                s.relayHost = GetText(GetDlgItem(hwnd, kIdEditRelayHost));
                s.relayPort = static_cast<uint16_t>(std::atoi(GetText(GetDlgItem(hwnd, kIdEditRelayPort)).c_str()));
                s.relayEnabled = IsChecked(hwnd, kIdCheckRelay) && !s.relayHost.empty();
                s.tunnelPort = static_cast<uint16_t>(std::atoi(GetText(GetDlgItem(hwnd, kIdEditTunnelPort)).c_str()));
                s.usbipdPath = GetText(GetDlgItem(hwnd, kIdEditUsbipd));
                s.usbipPath = GetText(GetDlgItem(hwnd, kIdEditUsbip));
                s.autoDetachOnExit = IsChecked(hwnd, kIdCheckDetach);
                s.verifyAttachWithSetupApi = IsChecked(hwnd, kIdCheckVerify);
                s.autoShareAll = IsChecked(hwnd, kIdCheckAutoShare);
                s.logLevel = std::atoi(GetText(GetDlgItem(hwnd, kIdEditLogLevel)).c_str());
                if (s.serverPort < 1024) s.serverPort = 4437;
                if (s.relayPort == 0) s.relayPort = 4443;
                if (s.tunnelPort < 1024) s.tunnelPort = 4240;
                if (s.codeTtlSeconds < 30) s.codeTtlSeconds = 300;
                std::string err;
                if (!s.Save(&err)) {
                    MessageBoxW(hwnd, W(err).c_str(), L"Settings", MB_OK | MB_ICONERROR);
                    return 0;
                }
                Logger::Instance().SetMinLevel(static_cast<LogLevel>(s.logLevel));
                ctx->saved = true;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            for (HWND h : ctx->controls) DestroyWindow(h);
            ctx->controls.clear();
            EnableWindow(ctx->parent, TRUE);
            SetForegroundWindow(ctx->parent);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

bool ShowSettings(HWND parent) {
    Settings& s = GetSettings();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(theme::kBg);
    wc.lpszClassName = L"MiniUsbBridgeSettings";
    RegisterClassExW(&wc);

    SettingsCtx ctx;
    ctx.parent = parent;
    ctx.font = reinterpret_cast<HFONT>(SendMessageW(parent, WM_GETFONT, 0, 0));
    if (!ctx.font) ctx.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    int w = 620, h = 640;
    RECT prc{};
    GetWindowRect(parent, &prc);
    int x = prc.left + ((prc.right - prc.left) - w) / 2;
    int y = prc.top + ((prc.bottom - prc.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MiniUsbBridgeSettings", L"Mini USB Bridge \x2014 Settings",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, parent, nullptr,
                               GetModuleHandleW(nullptr), &ctx);
    if (!dlg) return false;
    EnableWindow(parent, FALSE);

    int left = 20, fieldX = 210, fieldW = 380, row = 16, step = 34;
    AddLabel(&ctx, L"Server", left, row, 460);
    row += 22;
    AddLabel(&ctx, L"Direct TCP port", left, row + 3, 180);
    AddEdit(&ctx, kIdEditServerPort, std::to_string(s.serverPort), fieldX, row, 90);
    AddLabel(&ctx, L"Pairing code lifetime (s)", 320, row + 3, 200);
    AddEdit(&ctx, kIdEditTtl, std::to_string(s.codeTtlSeconds), 520, row, 70);
    row += step;
    AddLabel(&ctx, L"Public address / DDNS (optional)", left, row + 3, 180);
    AddEdit(&ctx, kIdEditPublicHost, s.publicHost, fieldX, row, fieldW);
    row += step;
    AddCheck(&ctx, kIdCheckSingleUse, L"Pairing code is single use (recommended)", s.singleUseCode, left, row, 420);
    row += 26;
    AddCheck(&ctx, kIdCheckAutoShare, L"Allow \u201cShare all\u201d without asking per device", s.autoShareAll, left,
             row, 460);
    row += 30;

    AddLabel(&ctx, L"Relay (NAT traversal)", left, row, 460);
    row += 22;
    AddCheck(&ctx, kIdCheckRelay, L"Use relay when a direct connection is impossible", s.relayEnabled, left, row, 460);
    row += 26;
    AddLabel(&ctx, L"Relay host", left, row + 3, 180);
    AddEdit(&ctx, kIdEditRelayHost, s.relayHost, fieldX, row, 230);
    AddLabel(&ctx, L"Port", 452, row + 3, 40);
    AddEdit(&ctx, kIdEditRelayPort, std::to_string(s.relayPort), 490, row, 70);
    row += step;

    AddLabel(&ctx, L"Client", left, row, 460);
    row += 22;
    AddLabel(&ctx, L"Local tunnel port", left, row + 3, 180);
    AddEdit(&ctx, kIdEditTunnelPort, std::to_string(s.tunnelPort), fieldX, row, 90);
    row += step;
    AddCheck(&ctx, kIdCheckDetach, L"Detach all devices when disconnecting", s.autoDetachOnExit, left, row, 460);
    row += 26;
    AddCheck(&ctx, kIdCheckVerify, L"Verify each attach in Device Manager (recommended)", s.verifyAttachWithSetupApi,
             left, row, 480);
    row += 30;

    AddLabel(&ctx, L"Backends", left, row, 460);
    row += 22;
    AddLabel(&ctx, L"usbipd.exe (server)", left, row + 3, 180);
    AddEdit(&ctx, kIdEditUsbipd, s.usbipdPath, fieldX, row, 230);
    AddButton(&ctx, kIdFirewall, L"Firewall...", 452, row, 90, 26);
    row += step;
    AddLabel(&ctx, L"usbip.exe (client)", left, row + 3, 180);
    AddEdit(&ctx, kIdEditUsbip, s.usbipPath, fieldX, row, fieldW);
    row += step;
    AddLabel(&ctx, L"Log level (0-3)", left, row + 3, 180);
    AddEdit(&ctx, kIdEditLogLevel, std::to_string(s.logLevel), fieldX, row, 60);

    AddButton(&ctx, kIdLogs, L"Open log folder", left, h - 92, 130, 28);
    AddButton(&ctx, kIdCancel, L"Cancel", w - 210, h - 92, 90, 28);
    AddButton(&ctx, kIdSave, L"Save", w - 110, h - 92, 90, 28);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return ctx.saved;
}

void ShowInfoBox(HWND parent, const wchar_t* title, const std::wstring& text, bool error) {
    MessageBoxW(parent, text.c_str(), title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

// --- approval dialog -------------------------------------------------------

namespace {

struct ApprovalCtx {
    HWND hwnd = nullptr;
    HWND parent = nullptr;
    HFONT font = nullptr;
    std::shared_ptr<ApprovalRequest> request;
    int secondsLeft = 60;
};

LRESULT CALLBACK ApprovalProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<ApprovalCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ctx = static_cast<ApprovalCtx*>(cs->lpCreateParams);
            ctx->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme::kText);
            static HBRUSH brush = CreateSolidBrush(theme::kCard);
            SetBkColor(dc, theme::kCard);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (!ctx || !ctx->request) return 0;
            if (id == 1) ctx->request->Decide(true);
            if (id == 2) ctx->request->Decide(false);
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_TIMER: {
            if (!ctx || !ctx->request) return 0;
            --ctx->secondsLeft;
            if (ctx->secondsLeft <= 0) {
                ctx->request->Decide(false);
                DestroyWindow(hwnd);
                return 0;
            }
            std::wstring txt = L"Automatic reject in " + std::to_wstring(ctx->secondsLeft) + L" s";
            SetDlgItemTextW(hwnd, 10, txt.c_str());
            return 0;
        }
        case WM_CLOSE:
            if (ctx && ctx->request) ctx->request->Decide(false);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            EnableWindow(ctx->parent, TRUE);
            SetForegroundWindow(ctx->parent);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

void ShowApprovalWindow(HWND parent, std::shared_ptr<ApprovalRequest> request) {
    if (!request) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ApprovalProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(theme::kCard);
    wc.lpszClassName = L"MiniUsbBridgeApproval";
    RegisterClassExW(&wc);

    ApprovalCtx ctx;
    ctx.parent = parent;
    ctx.request = request;
    ctx.font = reinterpret_cast<HFONT>(SendMessageW(parent, WM_GETFONT, 0, 0));
    if (!ctx.font) ctx.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    int w = 460, h = 270;
    RECT prc{};
    GetWindowRect(parent, &prc);
    int x = prc.left + ((prc.right - prc.left) - w) / 2;
    int y = prc.top + ((prc.bottom - prc.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"MiniUsbBridgeApproval",
                               L"Client access request", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h,
                               parent, nullptr, GetModuleHandleW(nullptr), &ctx);
    if (!dlg) return;
    EnableWindow(parent, FALSE);
    SetForegroundWindow(dlg);

    auto add = [&](const wchar_t* cls, const wchar_t* text, int id, int xx, int yy, int ww, int hh, DWORD style) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, xx, yy, ww, hh, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ctx.font), TRUE);
        return c;
    };

    add(L"STATIC", L"A client is requesting access to your USB devices.", -1, 18, 16, 420, 22, SS_LEFT);
    std::wstring detail = W("Client:  " + request->clientName + "\nFrom:  " + request->peer +
                            "\nPath:  " + request->path + "\nAccess:  " + request->requested);
    HWND info = add(L"STATIC", W(detail).c_str(), -1, 18, 46, 420, 100, SS_LEFT);
    SendMessageW(info, WM_SETFONT, reinterpret_cast<WPARAM>(ctx.font), TRUE);
    add(L"STATIC", L"Automatic reject in 60 s", 10, 18, 152, 300, 20, SS_LEFT);
    add(L"BUTTON", L"Reject", 2, 200, 190, 110, 32, BS_PUSHBUTTON | WS_TABSTOP);
    add(L"BUTTON", L"Allow", 1, 322, 190, 118, 32, BS_DEFPUSHBUTTON | WS_TABSTOP);
    SetTimer(dlg, 1, 1000, nullptr);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}  // namespace mub

#endif  // _WIN32
