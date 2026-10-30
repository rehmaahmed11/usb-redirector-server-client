// Mini USB Bridge - main window implementation (Win32, owner drawn)
// SPDX-License-Identifier: MIT
#include "main_window.h"

#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <dbt.h>
#include <initguid.h>
#include <usbiodef.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "../client/client_mode.h"
#include "../client/usbip_cli.h"
#include "../core/logger.h"
#include "../core/settings.h"
#include "../core/strings.h"
#include "../server/server_mode.h"
#include "../server/usb_detect.h"
#include "../server/usbipd_cli.h"
#include "../win/elevated.h"
#include "../win/process.h"
#include "../win/tls.h"
#include "settings_dialog.h"

namespace mub {
namespace {

constexpr int kIdModeServer = 101;
constexpr int kIdModeClient = 102;
constexpr int kIdBack = 103;
constexpr int kIdCopyCode = 104;
constexpr int kIdRegenCode = 105;
constexpr int kIdDisconnectClient = 106;
constexpr int kIdShareSel = 107;
constexpr int kIdStopShareSel = 108;
constexpr int kIdShareAll = 109;
constexpr int kIdStopShareAll = 110;
constexpr int kIdOpenLogs = 111;
constexpr int kIdSettings = 112;
constexpr int kIdPaste = 113;
constexpr int kIdConnect = 114;
constexpr int kIdRefresh = 115;
constexpr int kIdClientDisconnect = 116;
constexpr int kIdCodeEdit = 500;
constexpr int kIdRowBase = 1000;      // + index -> select row
constexpr int kIdRowActionBase = 2000;  // + index -> share/attach action
constexpr int kTimerTick = 1;
constexpr int kTimerDeviceChange = 2;

std::wstring W(const std::string& s) { return Utf8ToWide(s); }
std::wstring W(const char* s) { return Utf8ToWide(std::string(s)); }

COLORREF StatusColor(const std::string& text, bool online) {
    std::string t = Lower(text);
    if (t.find("error") != std::string::npos || t.find("lost") != std::string::npos ||
        t.find("reject") != std::string::npos || t.find("fail") != std::string::npos)
        return theme::kError;
    if (t.find("waiting") != std::string::npos || t.find("approval") != std::string::npos ||
        t.find("connecting") != std::string::npos || t.find("authenticating") != std::string::npos)
        return theme::kWarn;
    if (online) return theme::kSuccess;
    if (t.find("stopped") != std::string::npos || t.find("not connected") != std::string::npos)
        return theme::kTextFaint;
    return theme::kAccent;
}

}  // namespace

MainWindow& MainWindow::Instance() {
    static MainWindow inst;
    return inst;
}

bool MainWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWindow::WndProcStatic;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MiniUsbBridgeMainWindow";
    wc.hIcon = LoadIconW(instance, L"IDI_APP");
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    return RegisterClassExW(&wc) != 0;
}

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    if (!RegisterWindowClass(instance)) return false;

    dpi_ = 96;
    hwnd_ = CreateWindowExW(0, L"MiniUsbBridgeMainWindow", L"Mini USB Bridge",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 980, 730,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    UiQueue::Instance().SetWindow(hwnd_);
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTimerTick, 500, nullptr);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            dpi_ = GetDpiForWindow(hwnd);
            fontBase_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                                    L"Segoe UI");
            fontSmall_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                                     L"Segoe UI");
            fontBold_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    VARIABLE_PITCH, L"Segoe UI");
            fontTitle_ = CreateFontW(-MulDiv(19, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     VARIABLE_PITCH, L"Segoe UI");
            fontBig_ = CreateFontW(-MulDiv(26, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   VARIABLE_PITCH, L"Segoe UI");
            fontMono_ = CreateFontW(-MulDiv(11, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                                    L"Consolas");
            CreateChildControls();
            BackendCheck();
            // Live USB arrival/removal notifications (polling stays as a fallback,
            // because usbipd also needs a rescan when sharing state changes).
            DEV_BROADCAST_DEVICEINTERFACE_W filter{};
            filter.dbcc_size = sizeof(filter);
            filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
            deviceNotify_ = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = MulDiv(760, dpi_, 96);
            mmi->ptMinTrackSize.y = MulDiv(600, dpi_, 96);
            return 0;
        }
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case kUiEventMessage: {
            while (auto ev = UiQueue::Instance().Pop()) OnUiEvent(ev);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            UpdateHover(pt);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            pressedButton_ = -1;
            for (const auto& b : buttons_) {
                if (PtInRect(&b.rc, pt) && b.enabled) {
                    pressedButton_ = b.id;
                    break;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (const auto& b : buttons_) {
                if (PtInRect(&b.rc, pt) && b.enabled && b.id == pressedButton_) {
                    OnCommand(b.id);
                    break;
                }
            }
            pressedButton_ = -1;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = delta > 0 ? -1 : 1;
            if (mode_ == Mode::Server) {
                int maxScroll = static_cast<int>(serverRows_.size()) - layout_.visibleRows;
                serverScroll_ = std::max(0, std::min(serverScroll_ + step, std::max(0, maxScroll)));
            } else if (mode_ == Mode::Client) {
                int maxScroll = static_cast<int>(clientRows_.size()) - layout_.visibleRows;
                clientScroll_ = std::max(0, std::min(clientScroll_ + step, std::max(0, maxScroll)));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DEVICECHANGE: {
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                // Debounce: a single plug event often produces several notifications.
                SetTimer(hwnd, kTimerDeviceChange, 700, nullptr);
            }
            return TRUE;
        }
        case WM_TIMER: {
            if (wp == kTimerDeviceChange) {
                KillTimer(hwnd, kTimerDeviceChange);
                if (mode_ == Mode::Server && ServerMode::Instance().Running()) {
                    RunAsync([this]() {
                        ServerMode::Instance().RefreshDevices();
                        PostMessageW(hwnd_, kUiEventMessage, 0, 0);
                    });
                }
            }
            if (wp == kTimerTick) {
                if (!bannerText_.empty() && NowUnix() >= bannerUntil_) {
                    bannerText_.clear();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            RECT* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (fontBase_) DeleteObject(fontBase_);
            if (fontSmall_) DeleteObject(fontSmall_);
            if (fontBold_) DeleteObject(fontBold_);
            if (fontTitle_) DeleteObject(fontTitle_);
            if (fontBig_) DeleteObject(fontBig_);
            if (fontMono_) DeleteObject(fontMono_);
            fontBase_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                                    L"Segoe UI");
            fontSmall_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                                     L"Segoe UI");
            fontBold_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    VARIABLE_PITCH, L"Segoe UI");
            fontTitle_ = CreateFontW(-MulDiv(19, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     VARIABLE_PITCH, L"Segoe UI");
            fontBig_ = CreateFontW(-MulDiv(26, dpi_, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                                   L"Segoe UI");
            fontMono_ = CreateFontW(-MulDiv(11, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                                    L"Consolas");
            if (codeEdit_) SendMessageW(codeEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontMono_), TRUE);
            Layout();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == kIdCodeEdit && HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme::kMono);
            SetBkColor(dc, theme::kCard);
            static HBRUSH brush = CreateSolidBrush(theme::kCard);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_CLOSE: {
            if (ServerMode::Instance().Running()) ServerMode::Instance().Stop();
            if (ClientMode::Instance().Running()) ClientMode::Instance().Disconnect("window closed");
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTimerTick);
            if (deviceNotify_) {
                UnregisterDeviceNotification(deviceNotify_);
                deviceNotify_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void MainWindow::CreateChildControls() {
    codeEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCodeEdit)), instance_, nullptr);
    SendMessageW(codeEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(fontMono_), TRUE);
    SendMessageW(codeEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Paste the MUB1-... code here"));
    ShowWindow(codeEdit_, SW_HIDE);
}

void MainWindow::BackendCheck() {
    std::thread([this]() {
        auto usbipd = UsbipdCli::Instance().Probe(true);
        auto usbip = UsbipCli::Instance().Probe(true);
        bool serverOk = usbipd.installed;
        bool clientOk = usbip.installed && usbip.driverPresent;
        std::string serverText = serverOk ? ("usbipd-win found: " + usbipd.version)
                                          : "usbipd-win missing - needed to share devices (Server mode)";
        std::string clientText;
        if (!usbip.installed)
            clientText = "usbip-win2 missing - needed to attach devices (Client mode)";
        else if (!usbip.driverPresent)
            clientText = "usbip.exe found, but the usbip-win2 driver is not installed";
        else
            clientText = "usbip-win2 found: " + usbip.version;
        {
            std::lock_guard<std::mutex> lock(mu_);
            backendServerOk_ = serverOk;
            backendClientOk_ = clientOk;
            backendServerText_ = serverText;
            backendClientText_ = clientText;
        }
        if (!serverOk || !clientOk) {
            SetBanner(serverOk ? "Client backend incomplete: install usbip-win2 for Client mode."
                               : "Server backend missing: install usbipd-win for Server mode.",
                      "warn");
        }
        PostMessageW(hwnd_, kUiEventMessage, 0, 0);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }).detach();
}

// --- layout ---------------------------------------------------------------

RECT MainWindow::ContentRect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    rc.top += MulDiv(76, dpi_, 96);
    rc.left += MulDiv(18, dpi_, 96);
    rc.right -= MulDiv(18, dpi_, 96);
    rc.bottom -= MulDiv(14, dpi_, 96);
    return rc;
}

void MainWindow::Layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    int pad = MulDiv(18, dpi_, 96);
    int gap = MulDiv(14, dpi_, 96);
    int rowH = MulDiv(46, dpi_, 96);

    layout_.header = RECT{0, 0, client.right, MulDiv(76, dpi_, 96)};
    layout_.content = ContentRect();
    RECT content = layout_.content;

    layout_.back = RECT{content.left, content.top, content.left + MulDiv(92, dpi_, 96), content.top + MulDiv(30, dpi_, 96)};
    layout_.settingsBtn = RECT{content.right - MulDiv(104, dpi_, 96), content.top,
                               content.right, content.top + MulDiv(30, dpi_, 96)};
    int top = content.top + MulDiv(40, dpi_, 96);

    if (mode_ == Mode::Choose) {
        int cardH = MulDiv(196, dpi_, 96);
        int cardTop = content.top + MulDiv(84, dpi_, 96);
        int half = (content.right - content.left - gap) / 2;
        layout_.chooseServerCard = RECT{content.left, cardTop, content.left + half, cardTop + cardH};
        layout_.chooseClientCard = RECT{content.left + half + gap, cardTop, content.right, cardTop + cardH};
        layout_.backendCard = RECT{content.left, cardTop + cardH + gap, content.right,
                                   cardTop + cardH + gap + MulDiv(96, dpi_, 96)};
        buttons_.clear();
        auto add = [&](RECT rc, int id, const wchar_t* label, ButtonStyle st, bool enabled = true) {
            HitButton b;
            b.rc = rc;
            b.id = id;
            b.label = label;
            b.style = st;
            b.enabled = enabled;
            buttons_.push_back(b);
        };
        add(RECT{layout_.back.left, layout_.back.top, layout_.back.right, layout_.back.bottom}, kIdBack, L"",
            ButtonStyle::Ghost);
        add(RECT{layout_.chooseServerCard.left + pad, layout_.chooseServerCard.bottom - MulDiv(56, dpi_, 96),
                 layout_.chooseServerCard.left + pad + MulDiv(150, dpi_, 96),
                 layout_.chooseServerCard.bottom - MulDiv(22, dpi_, 96)},
            kIdModeServer, L"Use as SERVER", ButtonStyle::Primary);
        add(RECT{layout_.chooseClientCard.left + pad, layout_.chooseClientCard.bottom - MulDiv(56, dpi_, 96),
                 layout_.chooseClientCard.left + pad + MulDiv(150, dpi_, 96),
                 layout_.chooseClientCard.bottom - MulDiv(22, dpi_, 96)},
            kIdModeClient, L"Use as CLIENT", ButtonStyle::Primary);
        add(layout_.settingsBtn, kIdSettings, L"Settings", ButtonStyle::Secondary);
        ShowWindow(codeEdit_, SW_HIDE);
        return;
    }

    if (mode_ == Mode::Server) {
        int logH = MulDiv(34, dpi_, 96);
        int footerY = content.bottom - logH;
        layout_.logStrip = RECT{content.left, footerY, content.right, content.bottom};
        layout_.openLogBtn = RECT{content.right - MulDiv(96, dpi_, 96), footerY + MulDiv(4, dpi_, 96),
                                  content.right, footerY + MulDiv(28, dpi_, 96)};

        int topRowH = MulDiv(196, dpi_, 96);
        int leftW = (content.right - content.left) * 55 / 100;
        layout_.cardServer = RECT{content.left, top, content.left + leftW, top + topRowH};
        layout_.cardClient = RECT{content.left + leftW + gap, top, content.right, top + topRowH};

        layout_.codeBox = RECT{layout_.cardServer.left + pad, layout_.cardServer.top + MulDiv(46, dpi_, 96),
                               layout_.cardServer.right - pad, layout_.cardServer.top + MulDiv(96, dpi_, 96)};
        layout_.copyBtn = RECT{layout_.codeBox.left, layout_.codeBox.bottom + MulDiv(8, dpi_, 96),
                               layout_.codeBox.left + MulDiv(96, dpi_, 96),
                               layout_.codeBox.bottom + MulDiv(36, dpi_, 96)};
        layout_.regenBtn = RECT{layout_.copyBtn.right + MulDiv(8, dpi_, 96), layout_.copyBtn.top,
                                layout_.copyBtn.right + MulDiv(8, dpi_, 96) + MulDiv(132, dpi_, 96),
                                layout_.copyBtn.bottom};
        layout_.disconnectBtn = RECT{layout_.cardClient.left + pad,
                                     layout_.cardClient.bottom - MulDiv(42, dpi_, 96),
                                     layout_.cardClient.left + pad + MulDiv(150, dpi_, 96),
                                     layout_.cardClient.bottom - MulDiv(14, dpi_, 96)};

        int listTop = top + topRowH + gap;
        layout_.devicesCard = RECT{content.left, listTop, content.right, footerY - gap};
        int btnRowY = layout_.devicesCard.bottom - MulDiv(40, dpi_, 96);
        layout_.shareBtn = RECT{content.left + pad, btnRowY, content.left + pad + MulDiv(112, dpi_, 96),
                                btnRowY + MulDiv(28, dpi_, 96)};
        layout_.stopShareBtn = RECT{layout_.shareBtn.right + MulDiv(8, dpi_, 96), btnRowY,
                                    layout_.shareBtn.right + MulDiv(8, dpi_, 96) + MulDiv(124, dpi_, 96),
                                    btnRowY + MulDiv(28, dpi_, 96)};
        layout_.shareAllBtn = RECT{layout_.stopShareBtn.right + MulDiv(8, dpi_, 96), btnRowY,
                                   layout_.stopShareBtn.right + MulDiv(8, dpi_, 96) + MulDiv(112, dpi_, 96),
                                   btnRowY + MulDiv(28, dpi_, 96)};
        layout_.stopAllBtn = RECT{layout_.shareAllBtn.right + MulDiv(8, dpi_, 96), btnRowY,
                                  layout_.shareAllBtn.right + MulDiv(8, dpi_, 96) + MulDiv(112, dpi_, 96),
                                  btnRowY + MulDiv(28, dpi_, 96)};
        layout_.listArea = RECT{content.left + pad, layout_.devicesCard.top + MulDiv(56, dpi_, 96),
                                content.right - pad, btnRowY - MulDiv(8, dpi_, 96)};
        layout_.rowHeight = rowH;
        layout_.visibleRows = std::max(1, static_cast<int>((layout_.listArea.bottom - layout_.listArea.top) / rowH));

        buttons_.clear();
        auto add = [&](RECT rc, int id, const wchar_t* label, ButtonStyle st, bool enabled = true) {
            HitButton b;
            b.rc = rc;
            b.id = id;
            b.label = label;
            b.style = st;
            b.enabled = enabled;
            buttons_.push_back(b);
        };
        add(layout_.back, kIdBack, L"", ButtonStyle::Ghost);
        add(layout_.settingsBtn, kIdSettings, L"Settings", ButtonStyle::Secondary);
        add(layout_.copyBtn, kIdCopyCode, L"Copy code", ButtonStyle::Primary, !serverCode_.empty());
        add(layout_.regenBtn, kIdRegenCode, L"Regenerate", ButtonStyle::Secondary);
        add(layout_.disconnectBtn, kIdDisconnectClient, L"Disconnect client", ButtonStyle::Danger,
            serverClientOnline_);
        add(layout_.shareBtn, kIdShareSel, L"Share selected", ButtonStyle::Primary, serverSelection_ >= 0);
        add(layout_.stopShareBtn, kIdStopShareSel, L"Stop sharing", ButtonStyle::Secondary,
            serverSelection_ >= 0);
        add(layout_.shareAllBtn, kIdShareAll, L"Share all", ButtonStyle::Secondary);
        add(layout_.stopAllBtn, kIdStopShareAll, L"Stop all", ButtonStyle::Secondary);
        add(layout_.openLogBtn, kIdOpenLogs, L"View logs", ButtonStyle::Ghost);

        // row action buttons
        int shown = std::min<int>(layout_.visibleRows, static_cast<int>(serverRows_.size()) - serverScroll_);
        for (int i = 0; i < shown; ++i) {
            int idx = serverScroll_ + i;
            int y = layout_.listArea.top + i * rowH;
            RECT action{layout_.listArea.right - MulDiv(104, dpi_, 96), y + MulDiv(7, dpi_, 96),
                        layout_.listArea.right, y + rowH - MulDiv(7, dpi_, 96)};
            add(action, kIdRowActionBase + idx, serverRows_[idx].shared ? L"Stop" : L"Share",
                serverRows_[idx].shared ? ButtonStyle::RowActionOn : ButtonStyle::RowAction);
        }
        ShowWindow(codeEdit_, SW_HIDE);
        return;
    }

    // client view
    int footerY = content.bottom - MulDiv(34, dpi_, 96);
    layout_.logStrip = RECT{content.left, footerY, content.right, content.bottom};
    layout_.openLogBtn = RECT{content.right - MulDiv(96, dpi_, 96), footerY + MulDiv(4, dpi_, 96),
                              content.right, footerY + MulDiv(28, dpi_, 96)};
    int cardH = MulDiv(150, dpi_, 96);
    layout_.connectCard = RECT{content.left, top, content.right, top + cardH};
    layout_.inputBox = RECT{layout_.connectCard.left + pad, layout_.connectCard.top + MulDiv(58, dpi_, 96),
                            layout_.connectCard.right - pad - MulDiv(290, dpi_, 96),
                            layout_.connectCard.top + MulDiv(92, dpi_, 96)};
    layout_.pasteBtn = RECT{layout_.inputBox.right + MulDiv(8, dpi_, 96), layout_.inputBox.top,
                            layout_.inputBox.right + MulDiv(8, dpi_, 96) + MulDiv(76, dpi_, 96),
                            layout_.inputBox.bottom};
    layout_.connectBtn = RECT{layout_.pasteBtn.right + MulDiv(8, dpi_, 96), layout_.inputBox.top,
                              layout_.pasteBtn.right + MulDiv(8, dpi_, 96) + MulDiv(100, dpi_, 96),
                              layout_.inputBox.bottom};
    layout_.clientDisconnectBtn = RECT{layout_.connectBtn.right + MulDiv(8, dpi_, 96), layout_.inputBox.top,
                                       layout_.connectBtn.right + MulDiv(8, dpi_, 96) + MulDiv(104, dpi_, 96),
                                       layout_.inputBox.bottom};

    int devicesTop = layout_.connectCard.bottom + gap;
    layout_.clientDevicesCard = RECT{content.left, devicesTop, content.right, footerY - gap};
    layout_.refreshBtn = RECT{content.right - pad - MulDiv(112, dpi_, 96),
                              layout_.clientDevicesCard.top + MulDiv(10, dpi_, 96), content.right - pad,
                              layout_.clientDevicesCard.top + MulDiv(38, dpi_, 96)};
    layout_.listArea = RECT{content.left + pad, layout_.clientDevicesCard.top + MulDiv(48, dpi_, 96),
                            content.right - pad, layout_.clientDevicesCard.bottom - MulDiv(10, dpi_, 96)};
    layout_.rowHeight = rowH;
    layout_.visibleRows = std::max(1, static_cast<int>((layout_.listArea.bottom - layout_.listArea.top) / rowH));

    buttons_.clear();
    auto add = [&](RECT rc, int id, const wchar_t* label, ButtonStyle st, bool enabled = true) {
        HitButton b;
        b.rc = rc;
        b.id = id;
        b.label = label;
        b.style = st;
        b.enabled = enabled;
        buttons_.push_back(b);
    };
    add(layout_.back, kIdBack, L"", ButtonStyle::Ghost);
    add(layout_.settingsBtn, kIdSettings, L"Settings", ButtonStyle::Secondary);
    add(layout_.pasteBtn, kIdPaste, L"Paste", ButtonStyle::Secondary);
    add(layout_.connectBtn, kIdConnect, L"CONNECT", ButtonStyle::Primary, !ClientMode::Instance().Running());
    add(layout_.clientDisconnectBtn, kIdClientDisconnect, L"Disconnect", ButtonStyle::Danger,
        ClientMode::Instance().Running());
    add(layout_.refreshBtn, kIdRefresh, L"Refresh devices", ButtonStyle::Secondary,
        ClientMode::Instance().Running());
    add(layout_.openLogBtn, kIdOpenLogs, L"View logs", ButtonStyle::Ghost);

    int shown = std::min<int>(layout_.visibleRows, static_cast<int>(clientRows_.size()) - clientScroll_);
    for (int i = 0; i < shown; ++i) {
        int idx = clientScroll_ + i;
        int y = layout_.listArea.top + i * rowH;
        bool attached = false;
        auto it = clientAttach_.find(clientRows_[idx].busid);
        if (it != clientAttach_.end()) attached = it->second.first;
        RECT action{layout_.listArea.right - MulDiv(104, dpi_, 96), y + MulDiv(7, dpi_, 96),
                    layout_.listArea.right, y + rowH - MulDiv(7, dpi_, 96)};
        add(action, kIdRowActionBase + idx, attached ? L"DETACH" : L"ATTACH",
            attached ? ButtonStyle::RowActionOn : ButtonStyle::RowAction,
            clientRows_[idx].present && !clientRows_[idx].busid.empty());
    }

    if (mode_ == Mode::Client && codeEdit_) {
        MoveWindow(codeEdit_, layout_.inputBox.left + 2, layout_.inputBox.top + 2,
                   layout_.inputBox.right - layout_.inputBox.left - 4,
                   layout_.inputBox.bottom - layout_.inputBox.top - 4, TRUE);
        ShowWindow(codeEdit_, SW_SHOW);
        clientInputVisible_ = true;
    }
}

// --- painting -------------------------------------------------------------

void MainWindow::Paint(HDC dc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    ui::FillRectColor(mem, client, theme::kBg);

    PaintHeader(mem, layout_.header);
    if (mode_ == Mode::Choose)
        PaintChoose(mem, layout_.content);
    else if (mode_ == Mode::Server)
        PaintServer(mem, layout_.content);
    else
        PaintClient(mem, layout_.content);

    for (const auto& b : buttons_) PaintButton(mem, b);

    if (!bannerText_.empty()) PaintBanner(mem, layout_.content);
    if (mode_ != Mode::Choose) PaintLogStrip(mem, layout_.content);

    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::PaintHeader(HDC dc, RECT rc) {
    ui::FillRectColor(dc, rc, theme::kHeader);
    int x = rc.left + MulDiv(20, dpi_, 96);
    int y = rc.top + MulDiv(14, dpi_, 96);
    ui::DrawTextIn(dc, L"Mini USB Bridge", RECT{x, y, rc.right - MulDiv(240, dpi_, 96), y + MulDiv(26, dpi_, 96)},
                   fontTitle_, RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE);
    ui::DrawTextIn(dc, L"USB devices over the internet \u2022 end-to-end encrypted",
                   RECT{x, y + MulDiv(28, dpi_, 96), rc.right - MulDiv(240, dpi_, 96), y + MulDiv(48, dpi_, 96)},
                   fontSmall_, theme::kHeaderSub, DT_LEFT | DT_SINGLELINE);

    // status pill
    std::string status = mode_ == Mode::Server ? serverStatus_ : (mode_ == Mode::Client ? clientStatus_ : "Ready");
    bool online = mode_ == Mode::Server ? serverClientOnline_ : ClientMode::Instance().Running();
    COLORREF col = StatusColor(status, online);
    int pillW = MulDiv(240, dpi_, 96);
    RECT pill{rc.right - pillW - MulDiv(18, dpi_, 96), rc.top + MulDiv(22, dpi_, 96), rc.right - MulDiv(18, dpi_, 96),
              rc.top + MulDiv(52, dpi_, 96)};
    ui::FillRoundRect(dc, pill, MulDiv(15, dpi_, 96), RGB(0x1F, 0x2C, 0x45));
    ui::DrawStatusDot(dc, pill.left + MulDiv(14, dpi_, 96), (pill.top + pill.bottom) / 2, MulDiv(4, dpi_, 96), col);
    ui::DrawTextEllipsis(dc, W(status),
                         RECT{pill.left + MulDiv(24, dpi_, 96), pill.top, pill.right - MulDiv(10, dpi_, 96),
                              pill.bottom},
                         fontSmall_, RGB(0xE2, 0xE8, 0xF0), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::PaintChoose(HDC dc, RECT content) {
    int pad = MulDiv(18, dpi_, 96);
    ui::DrawTextIn(dc, L"How will this computer be used?",
                   RECT{content.left, content.top + MulDiv(44, dpi_, 96), content.right,
                        content.top + MulDiv(70, dpi_, 96)},
                   fontBold_, theme::kText, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextIn(dc,
                   L"Server = the PC with the USB device plugged in.  Client = the PC that needs to use it.",
                   RECT{content.left, content.top + MulDiv(68, dpi_, 96), content.right,
                        content.top + MulDiv(86, dpi_, 96)},
                   fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);

    auto drawCard = [&](const RECT& rc, const wchar_t* title, const wchar_t* sub, const wchar_t* bullet1,
                        const wchar_t* bullet2, COLORREF accent) {
        ui::FillRoundRect(dc, rc, MulDiv(10, dpi_, 96), theme::kCard);
        ui::FrameRoundRect(dc, rc, MulDiv(10, dpi_, 96), theme::kBorder);
        int x = rc.left + pad;
        int y = rc.top + MulDiv(18, dpi_, 96);
        ui::FillRoundRect(dc, RECT{x, y, x + MulDiv(36, dpi_, 96), y + MulDiv(36, dpi_, 96)},
                          MulDiv(18, dpi_, 96), accent);
        ui::DrawTextIn(dc, title, RECT{x + MulDiv(50, dpi_, 96), y + MulDiv(2, dpi_, 96), rc.right - pad,
                                       y + MulDiv(26, dpi_, 96)},
                       fontBold_, theme::kText, DT_LEFT | DT_SINGLELINE);
        ui::DrawTextIn(dc, sub,
                       RECT{x + MulDiv(50, dpi_, 96), y + MulDiv(24, dpi_, 96), rc.right - pad,
                            y + MulDiv(44, dpi_, 96)},
                       fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
        int by = y + MulDiv(62, dpi_, 96);
        ui::DrawTextIn(dc, bullet1,
                       RECT{x, by, rc.right - pad, by + MulDiv(20, dpi_, 96)}, fontSmall_, theme::kTextDim,
                       DT_LEFT | DT_SINGLELINE);
        ui::DrawTextIn(dc, bullet2,
                       RECT{x, by + MulDiv(20, dpi_, 96), rc.right - pad, by + MulDiv(40, dpi_, 96)}, fontSmall_,
                       theme::kTextDim, DT_LEFT | DT_SINGLELINE);
    };

    drawCard(layout_.chooseServerCard, L"SERVER", L"Share devices from this PC",
             L"\u2022  Detects USB devices automatically", L"\u2022  Generates a one-time pairing code",
             theme::kAccent);
    drawCard(layout_.chooseClientCard, L"CLIENT", L"Use a remote USB device here",
             L"\u2022  Paste the pairing code", L"\u2022  Attach the device to this PC", theme::kSuccess);

    // Backend readiness (the USB/IP engines are separate programs).
    ui::FillRoundRect(dc, layout_.backendCard, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.backendCard, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"USB-OVER-IP BACKENDS",
                   RECT{layout_.backendCard.left + pad, layout_.backendCard.top + MulDiv(10, dpi_, 96),
                        layout_.backendCard.right - pad, layout_.backendCard.top + MulDiv(28, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    auto backendLine = [&](int y, bool ok, const std::string& text) {
        ui::DrawStatusDot(dc, layout_.backendCard.left + pad + MulDiv(4, dpi_, 96), y + MulDiv(8, dpi_, 96),
                          MulDiv(4, dpi_, 96), ok ? theme::kSuccess : theme::kWarn);
        ui::DrawTextEllipsis(dc, W(text),
                             RECT{layout_.backendCard.left + pad + MulDiv(16, dpi_, 96), y,
                                  layout_.backendCard.right - pad, y + MulDiv(18, dpi_, 96)},
                             fontSmall_, ok ? theme::kTextDim : theme::kWarn, DT_LEFT | DT_SINGLELINE);
    };
    backendLine(layout_.backendCard.top + MulDiv(32, dpi_, 96), backendServerOk_, backendServerText_);
    backendLine(layout_.backendCard.top + MulDiv(52, dpi_, 96), backendClientOk_, backendClientText_);
    ui::DrawTextEllipsis(dc, L"Both backends are free open-source projects; this app adds the pairing, the "
                            L"approval and the encrypted tunnel.",
                         RECT{layout_.backendCard.left + pad, layout_.backendCard.top + MulDiv(72, dpi_, 96),
                              layout_.backendCard.right - pad, layout_.backendCard.bottom - MulDiv(4, dpi_, 96)},
                         fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
}

void MainWindow::PaintServer(HDC dc, RECT content) {
    (void)content;
    int pad = MulDiv(16, dpi_, 96);
    int small = MulDiv(9, dpi_, 96);

    // --- pairing code card ---
    ui::FillRoundRect(dc, layout_.cardServer, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.cardServer, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"PAIRING CODE",
                   RECT{layout_.cardServer.left + pad, layout_.cardServer.top + MulDiv(12, dpi_, 96),
                        layout_.cardServer.right - pad, layout_.cardServer.top + MulDiv(30, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    if (serverCode_.empty()) {
        ui::DrawTextIn(dc, L"Not available - start the server first",
                       RECT{layout_.codeBox.left, layout_.codeBox.top, layout_.codeBox.right, layout_.codeBox.bottom},
                       fontBase_, theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        ui::FillRoundRect(dc, layout_.codeBox, MulDiv(6, dpi_, 96), theme::kCardAlt);
        ui::FrameRoundRect(dc, layout_.codeBox, MulDiv(6, dpi_, 96), theme::kBorder);
        RECT textRc{layout_.codeBox.left + MulDiv(8, dpi_, 96), layout_.codeBox.top + MulDiv(4, dpi_, 96),
                    layout_.codeBox.right - MulDiv(8, dpi_, 96), layout_.codeBox.bottom};
        // Long codes (DNS name in the code) wrap instead of being cut off.
        RECT calc = textRc;
        ui::DrawTextIn(dc, W(serverCode_), calc, fontMono_, theme::kMono, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        int textH = calc.bottom - calc.top;
        int boxH = textRc.bottom - textRc.top;
        if (textH < boxH) {
            int offset = (boxH - textH) / 2;
            textRc.top += offset;
            textRc.bottom = textRc.top + textH;
        }
        ui::DrawTextIn(dc, W(serverCode_), textRc, fontMono_, theme::kMono, DT_LEFT | DT_WORDBREAK);
    }
    std::string expiry = serverExpiry_ ? ("Expires in " + FormatCountdown(NowUnix(), serverExpiry_)) : "";
    const int line1Top = layout_.copyBtn.bottom + MulDiv(6, dpi_, 96);
    const int line2Top = line1Top + MulDiv(15, dpi_, 96);
    ui::DrawTextEllipsis(dc, W(expiry + "   \u2022   " + serverStatus_),
                         RECT{layout_.cardServer.left + pad, line1Top, layout_.cardServer.right - pad,
                              line1Top + MulDiv(14, dpi_, 96)},
                         fontSmall_, theme::kTextDim, DT_LEFT | DT_TOP | DT_SINGLELINE);
    // Which address the client will dial, and whether a friend on another network
    // can actually use this code. Right-aligned so it never pushes the address off.
    const bool reachWarn = !serverNote_.empty() && serverNote_.rfind("LAN only", 0) == 0;
    ui::DrawTextEllipsis(dc, W("Address " + (serverEndpoint_.empty() ? std::string("-") : serverEndpoint_)),
                         RECT{layout_.cardServer.left + pad, line2Top,
                              layout_.cardServer.left + pad + MulDiv(300, dpi_, 96), line2Top + MulDiv(14, dpi_, 96)},
                         fontSmall_, theme::kTextFaint, DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (!serverNote_.empty()) {
        ui::DrawTextEllipsis(dc, W(serverNote_),
                             RECT{layout_.cardServer.left + pad + MulDiv(300, dpi_, 96), line2Top,
                                  layout_.cardServer.right - pad, line2Top + MulDiv(14, dpi_, 96)},
                             fontSmall_, reachWarn ? theme::kWarn : theme::kSuccess,
                             DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }

    // --- client card ---
    ui::FillRoundRect(dc, layout_.cardClient, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.cardClient, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"CONNECTED CLIENT",
                   RECT{layout_.cardClient.left + pad, layout_.cardClient.top + MulDiv(12, dpi_, 96),
                        layout_.cardClient.right - pad, layout_.cardClient.top + MulDiv(30, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextEllipsis(dc, W(serverClient_),
                         RECT{layout_.cardClient.left + pad, layout_.cardClient.top + MulDiv(38, dpi_, 96),
                              layout_.cardClient.right - pad, layout_.cardClient.top + MulDiv(58, dpi_, 96)},
                         fontBold_, theme::kText, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextEllipsis(dc, W("Endpoint: " + serverEndpoint_),
                         RECT{layout_.cardClient.left + pad, layout_.cardClient.top + MulDiv(60, dpi_, 96),
                              layout_.cardClient.right - pad, layout_.cardClient.top + MulDiv(78, dpi_, 96)},
                         fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextEllipsis(dc, L"Approval is always required before a client can use your devices.",
                         RECT{layout_.cardClient.left + pad, layout_.cardClient.top + MulDiv(80, dpi_, 96),
                              layout_.cardClient.right - pad, layout_.cardClient.top + MulDiv(96, dpi_, 96)},
                         fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);

    // --- device list card ---
    ui::FillRoundRect(dc, layout_.devicesCard, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.devicesCard, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"USB DEVICES ON THIS PC",
                   RECT{layout_.devicesCard.left + pad, layout_.devicesCard.top + MulDiv(13, dpi_, 96),
                        layout_.devicesCard.right - pad - MulDiv(120, dpi_, 96),
                        layout_.devicesCard.top + MulDiv(31, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    std::string count = std::to_string(serverRows_.size()) + " detected";
    ui::DrawTextIn(dc, W(count),
                   RECT{layout_.devicesCard.right - pad - MulDiv(160, dpi_, 96),
                        layout_.devicesCard.top + MulDiv(13, dpi_, 96), layout_.devicesCard.right - pad,
                        layout_.devicesCard.top + MulDiv(31, dpi_, 96)},
                   fontSmall_, theme::kTextDim, DT_RIGHT | DT_SINGLELINE);

    // header row
    RECT hdr{layout_.listArea.left, layout_.listArea.top - MulDiv(18, dpi_, 96), layout_.listArea.right,
             layout_.listArea.top - MulDiv(2, dpi_, 96)};
    ui::DrawTextIn(dc, L"DEVICE",
                   RECT{hdr.left, hdr.top, hdr.left + (hdr.right - hdr.left) * 46 / 100, hdr.bottom}, fontSmall_,
                   theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextIn(dc, L"VID:PID",
                   RECT{hdr.left + (hdr.right - hdr.left) * 46 / 100, hdr.top,
                        hdr.left + (hdr.right - hdr.left) * 60 / 100, hdr.bottom},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextIn(dc, L"STATUS",
                   RECT{hdr.left + (hdr.right - hdr.left) * 60 / 100, hdr.top, hdr.right - MulDiv(110, dpi_, 96),
                        hdr.bottom},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);

    if (serverRows_.empty()) {
        ui::DrawTextIn(dc, L"No USB devices detected. Connect a device and it appears here automatically.",
                       RECT{layout_.listArea.left, layout_.listArea.top,
                            layout_.listArea.right - MulDiv(110, dpi_, 96),
                            layout_.listArea.top + MulDiv(40, dpi_, 96)},
                       fontSmall_, theme::kTextDim, DT_LEFT | DT_WORDBREAK);
    }

    int shown = std::min<int>(layout_.visibleRows, static_cast<int>(serverRows_.size()) - serverScroll_);
    for (int i = 0; i < shown; ++i) {
        int idx = serverScroll_ + i;
        const UsbDeviceRow& row = serverRows_[idx];
        int y = layout_.listArea.top + i * layout_.rowHeight;
        RECT rowRc{layout_.listArea.left, y, layout_.listArea.right, y + layout_.rowHeight};
        bool selected = idx == serverSelection_;
        if (selected) ui::FillRoundRect(dc, rowRc, MulDiv(6, dpi_, 96), theme::kAccentSoft);
        else if (i % 2 == 1) ui::FillRoundRect(dc, rowRc, MulDiv(6, dpi_, 96), theme::kCardAlt);
        ui::FillRectColor(dc, RECT{rowRc.left, rowRc.bottom - 1, rowRc.right, rowRc.bottom}, theme::kBorder);

        int textX = rowRc.left + MulDiv(8, dpi_, 96);
        int nameW = (rowRc.right - rowRc.left) * 46 / 100;
        ui::DrawTextEllipsis(dc, W(row.name),
                             RECT{textX, y + MulDiv(6, dpi_, 96), rowRc.left + nameW, y + MulDiv(24, dpi_, 96)},
                             fontBold_, theme::kText, DT_LEFT | DT_SINGLELINE);
        std::string sub = row.manufacturer.empty() ? row.busid : (row.manufacturer + "  \u2022  bus " + row.busid);
        ui::DrawTextEllipsis(dc, W(sub),
                             RECT{textX, y + MulDiv(24, dpi_, 96), rowRc.left + nameW, y + rowRc.bottom - y - 4},
                             fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
        ui::DrawTextIn(dc, W(row.vidpid),
                       RECT{rowRc.left + (rowRc.right - rowRc.left) * 46 / 100, y,
                            rowRc.left + (rowRc.right - rowRc.left) * 60 / 100, y + rowRc.bottom - y},
                       fontMono_, theme::kMono, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::string statusText = row.present ? (row.shared ? "Shared" : "Ready") : "Removed";
        COLORREF statusColor = !row.present ? theme::kTextFaint : (row.shared ? theme::kSuccess : theme::kTextDim);
        RECT badge{rowRc.left + (rowRc.right - rowRc.left) * 60 / 100, y + MulDiv(13, dpi_, 96),
                   rowRc.left + (rowRc.right - rowRc.left) * 60 / 100 + MulDiv(74, dpi_, 96),
                   y + MulDiv(33, dpi_, 96)};
        ui::DrawBadge(dc, badge, W(statusText), fontSmall_,
                      row.shared ? theme::kSuccessSoft : theme::kCardAlt, statusColor);
    }
    if (static_cast<int>(serverRows_.size()) > layout_.visibleRows) {
        ui::DrawTextIn(dc, W("Scroll with the mouse wheel (" + std::to_string(serverRows_.size()) + " devices)"),
                       RECT{layout_.listArea.right - MulDiv(220, dpi_, 96),
                            layout_.devicesCard.bottom - MulDiv(36, dpi_, 96), layout_.listArea.right,
                            layout_.devicesCard.bottom - MulDiv(16, dpi_, 96)},
                       fontSmall_, theme::kTextFaint, DT_RIGHT | DT_SINGLELINE);
    }
    (void)small;
}

void MainWindow::PaintClient(HDC dc, RECT content) {
    (void)content;
    int pad = MulDiv(16, dpi_, 96);

    ui::FillRoundRect(dc, layout_.connectCard, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.connectCard, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"ENTER SERVER PAIRING CODE",
                   RECT{layout_.connectCard.left + pad, layout_.connectCard.top + MulDiv(14, dpi_, 96),
                        layout_.connectCard.right - pad, layout_.connectCard.top + MulDiv(32, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);
    ui::DrawTextIn(dc, L"Ask the server user to press \u2018Copy code\u2019, then paste it here.",
                   RECT{layout_.connectCard.left + pad, layout_.connectCard.top + MulDiv(32, dpi_, 96),
                        layout_.connectCard.right - pad, layout_.connectCard.top + MulDiv(50, dpi_, 96)},
                   fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
    ui::FrameRoundRect(dc, layout_.inputBox, MulDiv(6, dpi_, 96), theme::kBorder);
    ui::DrawTextEllipsis(dc, W(clientStatus_),
                         RECT{layout_.connectCard.left + pad, layout_.inputBox.bottom + MulDiv(10, dpi_, 96),
                              layout_.connectCard.right - pad, layout_.inputBox.bottom + MulDiv(32, dpi_, 96)},
                         fontSmall_, StatusColor(clientStatus_, ClientMode::Instance().Running()),
                         DT_LEFT | DT_SINGLELINE);
    if (!clientServer_.empty()) {
        ui::DrawTextEllipsis(dc, W(clientServer_),
                             RECT{layout_.connectCard.right - MulDiv(400, dpi_, 96),
                                  layout_.inputBox.bottom + MulDiv(10, dpi_, 96), layout_.connectCard.right - pad,
                                  layout_.inputBox.bottom + MulDiv(32, dpi_, 96)},
                             fontSmall_, theme::kTextDim, DT_RIGHT | DT_SINGLELINE);
    }

    ui::FillRoundRect(dc, layout_.clientDevicesCard, MulDiv(10, dpi_, 96), theme::kCard);
    ui::FrameRoundRect(dc, layout_.clientDevicesCard, MulDiv(10, dpi_, 96), theme::kBorder);
    ui::DrawTextIn(dc, L"SHARED DEVICES ON THE SERVER",
                   RECT{layout_.clientDevicesCard.left + pad, layout_.clientDevicesCard.top + MulDiv(13, dpi_, 96),
                        layout_.clientDevicesCard.right - pad - MulDiv(140, dpi_, 96),
                        layout_.clientDevicesCard.top + MulDiv(31, dpi_, 96)},
                   fontSmall_, theme::kTextFaint, DT_LEFT | DT_SINGLELINE);

    if (clientRows_.empty()) {
        ui::DrawTextIn(dc, L"Connect to a server to see the devices it shares.",
                       RECT{layout_.listArea.left, layout_.listArea.top, layout_.listArea.right,
                            layout_.listArea.top + MulDiv(40, dpi_, 96)},
                       fontSmall_, theme::kTextDim, DT_LEFT | DT_WORDBREAK);
    }
    int shown = std::min<int>(layout_.visibleRows, static_cast<int>(clientRows_.size()) - clientScroll_);
    for (int i = 0; i < shown; ++i) {
        int idx = clientScroll_ + i;
        const UsbDeviceRow& row = clientRows_[idx];
        int y = layout_.listArea.top + i * layout_.rowHeight;
        RECT rowRc{layout_.listArea.left, y, layout_.listArea.right, y + layout_.rowHeight};
        if (idx == clientSelection_) ui::FillRoundRect(dc, rowRc, MulDiv(6, dpi_, 96), theme::kAccentSoft);
        else if (i % 2 == 1) ui::FillRoundRect(dc, rowRc, MulDiv(6, dpi_, 96), theme::kCardAlt);
        ui::FillRectColor(dc, RECT{rowRc.left, rowRc.bottom - 1, rowRc.right, rowRc.bottom}, theme::kBorder);

        bool attached = false;
        auto it = clientAttach_.find(row.busid);
        if (it != clientAttach_.end()) attached = it->second.first;

        int textX = rowRc.left + MulDiv(8, dpi_, 96);
        int nameW = (rowRc.right - rowRc.left) * 50 / 100;
        ui::DrawTextEllipsis(dc, W(row.name),
                             RECT{textX, y + MulDiv(6, dpi_, 96), rowRc.left + nameW, y + MulDiv(24, dpi_, 96)},
                             fontBold_, theme::kText, DT_LEFT | DT_SINGLELINE);
        std::string sub = row.manufacturer.empty() ? ("bus " + row.busid) : (row.manufacturer + "  \u2022  bus " + row.busid);
        ui::DrawTextEllipsis(dc, W(sub),
                             RECT{textX, y + MulDiv(24, dpi_, 96), rowRc.left + nameW, y + rowRc.bottom - y - 4},
                             fontSmall_, theme::kTextDim, DT_LEFT | DT_SINGLELINE);
        ui::DrawTextIn(dc, W(row.vidpid),
                       RECT{rowRc.left + (rowRc.right - rowRc.left) * 50 / 100, y,
                            rowRc.left + (rowRc.right - rowRc.left) * 62 / 100, y + rowRc.bottom - y},
                       fontMono_, theme::kMono, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::string st = attached ? "Attached" : (row.present ? "Available" : "Removed");
        RECT badge{rowRc.left + (rowRc.right - rowRc.left) * 62 / 100, y + MulDiv(13, dpi_, 96),
                   rowRc.left + (rowRc.right - rowRc.left) * 62 / 100 + MulDiv(78, dpi_, 96),
                   y + MulDiv(33, dpi_, 96)};
        ui::DrawBadge(dc, badge, W(st), fontSmall_, attached ? theme::kSuccessSoft : theme::kCardAlt,
                      attached ? theme::kSuccess : theme::kTextDim);
    }
}

void MainWindow::PaintBanner(HDC dc, RECT content) {
    int h = MulDiv(30, dpi_, 96);
    RECT rc{content.left, content.top - MulDiv(36, dpi_, 96), content.right, content.top - MulDiv(36, dpi_, 96) + h};
    COLORREF bg = theme::kAccentSoft;
    COLORREF fg = theme::kAccent;
    if (bannerSeverity_ == "warn") {
        bg = theme::kWarnSoft;
        fg = theme::kWarn;
    } else if (bannerSeverity_ == "error") {
        bg = theme::kErrorSoft;
        fg = theme::kError;
    }
    ui::FillRoundRect(dc, rc, MulDiv(6, dpi_, 96), bg);
    ui::FrameRoundRect(dc, rc, MulDiv(6, dpi_, 96), fg);
    ui::DrawTextEllipsis(dc, W(bannerText_), RECT{rc.left + MulDiv(10, dpi_, 96), rc.top,
                                                  rc.right - MulDiv(10, dpi_, 96), rc.bottom},
                         fontSmall_, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::PaintLogStrip(HDC dc, RECT content) {
    uint64_t next = 0;
    auto entries = Logger::Instance().Snapshot(Logger::Instance().Sequence() > 3 ? Logger::Instance().Sequence() - 3 : 0,
                                               next);
    std::wstring line;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!line.empty()) line += L"   \u2022   ";
        line += W(entries[i].tag + ": " + entries[i].text);
    }
    if (line.empty()) line = L"Activity log is empty";
    ui::DrawTextEllipsis(dc, line,
                         RECT{content.left, layout_.logStrip.top, layout_.openLogBtn.left - MulDiv(10, dpi_, 96),
                              layout_.logStrip.bottom},
                         fontSmall_, theme::kTextFaint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::PaintButton(HDC dc, const HitButton& b) {
    COLORREF bg = theme::kCard;
    COLORREF fg = theme::kText;
    COLORREF border = theme::kBorder;
    bool filled = false;
    switch (b.style) {
        case ButtonStyle::Primary:
            bg = b.enabled ? (b.hover ? theme::kAccentDark : theme::kAccent) : RGB(0xC7, 0xD2, 0xE0);
            fg = RGB(255, 255, 255);
            filled = true;
            break;
        case ButtonStyle::Secondary:
            bg = b.hover ? theme::kAccentSoft : theme::kCard;
            fg = b.enabled ? theme::kText : theme::kTextFaint;
            border = theme::kBorder;
            break;
        case ButtonStyle::Danger:
            bg = b.hover ? theme::kErrorSoft : theme::kCard;
            fg = b.enabled ? theme::kError : theme::kTextFaint;
            border = b.enabled ? RGB(0xF2, 0xC5, 0xC5) : theme::kBorder;
            break;
        case ButtonStyle::Ghost:
            bg = b.hover ? theme::kCardAlt : theme::kBg;
            fg = theme::kTextDim;
            border = theme::kBg;
            break;
        case ButtonStyle::RowAction:
            bg = b.hover ? theme::kAccent : theme::kAccentSoft;
            fg = b.hover ? RGB(255, 255, 255) : theme::kAccent;
            border = theme::kAccentSoft;
            break;
        case ButtonStyle::RowActionOn:
            bg = b.hover ? RGB(0xDC, 0x26, 0x26) : theme::kCardAlt;
            fg = b.hover ? RGB(255, 255, 255) : theme::kTextDim;
            border = theme::kBorder;
            break;
    }
    if (filled) {
        ui::FillRoundRect(dc, b.rc, b.radius, bg);
    } else {
        ui::FillRoundRect(dc, b.rc, b.radius, bg);
        ui::FrameRoundRect(dc, b.rc, b.radius, border);
    }
    if (b.id == kIdBack) {
        // drawn as a small "back" affordance
        ui::DrawTextIn(dc, L"\x2190  Back", b.rc, fontSmall_, theme::kTextDim,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    HFONT f = (b.style == ButtonStyle::Primary || b.style == ButtonStyle::RowAction ||
               b.style == ButtonStyle::RowActionOn)
                  ? fontBold_
                  : fontBase_;
    ui::DrawTextIn(dc, b.label, b.rc, f, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// --- interaction ----------------------------------------------------------

void MainWindow::UpdateHover(POINT pt) {
    int newHot = -1;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (PtInRect(&buttons_[i].rc, pt) && buttons_[i].enabled) {
            newHot = static_cast<int>(i);
            break;
        }
    }
    if (newHot != hotButton_) {
        hotButton_ = newHot;
        for (size_t i = 0; i < buttons_.size(); ++i) buttons_[i].hover = (static_cast<int>(i) == newHot);
        SetCursor(LoadCursorW(nullptr, newHot >= 0 ? IDC_HAND : IDC_ARROW));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::SwitchMode(Mode mode) {
    mode_ = mode;
    if (mode == Mode::Server) {
        serverStatus_ = "Starting Server...";
        std::string err;
        if (ServerMode::Instance().Running() || ServerMode::Instance().Start(&err)) {
            serverCode_ = ServerMode::Instance().CodeText();
            serverExpiry_ = ServerMode::Instance().CodeExpiresUnix();
            serverEndpoint_ = ServerMode::Instance().EndpointText();
            serverNote_ = ServerMode::Instance().ReachabilityNote();
            serverStatus_ = ServerMode::Instance().StateText();
        } else {
            serverStatus_ = "Error";
            SetBanner(err, "error");
        }
        ServerMode::Instance().RefreshDevices();
    } else if (mode == Mode::Client) {
        clientStatus_ = ClientMode::Instance().StateText();
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::OnCommand(int id) {
    if (id == kIdModeServer) {
        SwitchMode(Mode::Server);
        return;
    }
    if (id == kIdModeClient) {
        SwitchMode(Mode::Client);
        return;
    }
    if (id == kIdBack) {
        if (ClientMode::Instance().Running()) ClientMode::Instance().Disconnect("back to start");
        if (ServerMode::Instance().Running()) ServerMode::Instance().Stop();
        mode_ = Mode::Choose;
        serverStatus_ = "Stopped";
        clientStatus_ = "Not connected";
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id == kIdSettings) {
        ShowSettingsDialog();
        return;
    }
    if (id == kIdOpenLogs) {
        std::string dir = DataDir() + "\\logs";
        ShellExecuteA(hwnd_, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == kIdCopyCode) {
        std::string code = ServerMode::Instance().CodeText();
        if (!code.empty()) {
            SetClipboardText(code);
            SetBanner("Pairing code copied to the clipboard. Send it to the client user.", "info");
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return;
    }
    if (id == kIdRegenCode) {
        ServerMode::Instance().RegenerateCode();
        serverCode_ = ServerMode::Instance().CodeText();
        serverExpiry_ = ServerMode::Instance().CodeExpiresUnix();
        SetBanner("A new pairing code was generated. The previous code is now invalid.", "info");
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id == kIdDisconnectClient) {
        ServerMode::Instance().DisconnectClient("disconnected by the server user");
        serverClientOnline_ = false;
        serverClient_ = "No client connected";
        serverCode_ = ServerMode::Instance().CodeText();
        serverExpiry_ = ServerMode::Instance().CodeExpiresUnix();
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id == kIdShareAll) {
        RunAsync([this]() {
            std::string msg;
            bool ok = ServerMode::Instance().ShareAll(msg);
            SetBanner(msg, ok ? "info" : "error");
            PostMessageW(hwnd_, kUiEventMessage, 0, 0);
        });
        return;
    }
    if (id == kIdStopShareAll) {
        RunAsync([this]() {
            std::string msg;
            bool ok = ServerMode::Instance().StopSharingAll(msg);
            SetBanner(msg, ok ? "info" : "error");
            PostMessageW(hwnd_, kUiEventMessage, 0, 0);
        });
        return;
    }
    if (id == kIdShareSel || id == kIdStopShareSel) {
        if (serverSelection_ < 0 || serverSelection_ >= static_cast<int>(serverRows_.size())) return;
        UsbDeviceRow row = serverRows_[serverSelection_];
        bool share = (id == kIdShareSel);
        RunAsync([this, row, share]() {
            std::string msg;
            bool ok = share ? ServerMode::Instance().Share(row.busid, msg)
                            : ServerMode::Instance().StopSharing(row.busid, msg);
            SetBanner(msg, ok ? "info" : "error");
            PostMessageW(hwnd_, kUiEventMessage, 0, 0);
        });
        return;
    }
    if (id == kIdPaste) {
        std::string text = Trim(ClipboardText());
        SetWindowTextW(codeEdit_, W(text).c_str());
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id == kIdConnect) {
        wchar_t buf[1024] = {};
        GetWindowTextW(codeEdit_, buf, 1024);
        std::string code = WideToUtf8(buf);
        if (Trim(code).empty()) {
            SetBanner("Paste the pairing code from the server first.", "warn");
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        std::string err;
        if (!ClientMode::Instance().Connect(code, &err)) {
            SetBanner(err, "error");
            clientStatus_ = ClientMode::Instance().StateText();
        } else {
            clientStatus_ = "Connecting to server...";
        }
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id == kIdClientDisconnect) {
        ClientMode::Instance().Disconnect("disconnect button");
        clientStatus_ = "Not connected";
        clientServer_.clear();
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id == kIdRefresh) {
        std::string err;
        if (!ClientMode::Instance().RefreshDevices(&err)) SetBanner(err, "error");
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    if (id >= kIdRowActionBase) {
        int index = id - kIdRowActionBase;
        if (mode_ == Mode::Server) {
            if (index < 0 || index >= static_cast<int>(serverRows_.size())) return;
            UsbDeviceRow row = serverRows_[index];
            bool share = !row.shared;
            RunAsync([this, row, share]() {
                std::string msg;
                bool ok = share ? ServerMode::Instance().Share(row.busid, msg)
                                : ServerMode::Instance().StopSharing(row.busid, msg);
                SetBanner(msg, ok ? "info" : "error");
                PostMessageW(hwnd_, kUiEventMessage, 0, 0);
            });
        } else {
            if (index < 0 || index >= static_cast<int>(clientRows_.size())) return;
            UsbDeviceRow row = clientRows_[index];
            bool attached = false;
            auto it = clientAttach_.find(row.busid);
            if (it != clientAttach_.end()) attached = it->second.first;
            // Build the protocol entry the engine needs.
            DeviceEntry dev;
            dev.busid = row.busid;
            dev.name = row.name;
            dev.manufacturer = row.manufacturer;
            dev.vid = row.vid;
            dev.pid = row.pid;
            if (attached) {
                RunAsync([this, dev]() {
                    std::string msg;
                    ClientMode::Instance().DetachDevice(dev, msg);
                    PostMessageW(hwnd_, kUiEventMessage, 0, 0);
                });
            } else {
                SetBanner("Attaching " + row.name + " ...", "info");
                InvalidateRect(hwnd_, nullptr, TRUE);
                RunAsync([this, dev]() {
                    std::string msg;
                    ClientMode::Instance().AttachDevice(dev, msg);
                    PostMessageW(hwnd_, kUiEventMessage, 0, 0);
                });
            }
        }
        return;
    }
}

void MainWindow::RunAsync(std::function<void()> fn) {
    std::thread([fn]() { fn(); }).detach();
}

void MainWindow::SetBanner(const std::string& text, const std::string& severity) {
    bannerText_ = text;
    bannerSeverity_ = severity;
    bannerUntil_ = NowUnix() + 12;
    PostMessageW(hwnd_, kUiEventMessage, 0, 0);
    if (!text.empty()) LogI("ui", text);
}

std::string MainWindow::ClipboardText() const {
    if (!OpenClipboard(hwnd_)) return {};
    std::string out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        auto* p = static_cast<wchar_t*>(GlobalLock(h));
        if (p) {
            out = WideToUtf8(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void MainWindow::SetClipboardText(const std::string& text) const {
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    std::wstring w = W(text);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
        if (dst) {
            memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

void MainWindow::ApplyServerDevices(const std::vector<DeviceEntry>& devices) {
    serverRows_.clear();
    for (const auto& d : devices) {
        UsbDeviceRow row;
        row.busid = d.busid;
        row.name = d.name;
        row.manufacturer = d.manufacturer;
        row.vidpid = HexU16(d.vid) + ":" + HexU16(d.pid);
        row.vid = d.vid;
        row.pid = d.pid;
        row.shared = d.shared != 0;
        row.attached = d.attached != 0;
        row.present = d.state == 0;
        serverRows_.push_back(row);
    }
    if (serverSelection_ >= static_cast<int>(serverRows_.size())) serverSelection_ = -1;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplyClientDevices(const std::vector<DeviceEntry>& devices) {
    clientRows_.clear();
    for (const auto& d : devices) {
        UsbDeviceRow row;
        row.busid = d.busid;
        row.name = d.name;
        row.manufacturer = d.manufacturer;
        row.vidpid = HexU16(d.vid) + ":" + HexU16(d.pid);
        row.vid = d.vid;
        row.pid = d.pid;
        row.present = d.state == 0;
        clientRows_.push_back(row);
    }
    {
        auto states = ClientMode::Instance().AttachStates();
        clientAttach_.clear();
        for (const auto& kv : states) clientAttach_[kv.first] = {kv.second.attached, kv.second.busy};
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RefreshStatusPill() { InvalidateRect(hwnd_, nullptr, FALSE); }

void MainWindow::OnUiEvent(std::shared_ptr<UiEvent> ev) {
    if (!ev) return;
    switch (ev->kind) {
        case UiKind::ServerState:
            serverStatus_ = ev->a;
            serverExpiry_ = ServerMode::Instance().CodeExpiresUnix();
            serverCode_ = ServerMode::Instance().CodeText();
            serverEndpoint_ = ServerMode::Instance().EndpointText();
            serverNote_ = ServerMode::Instance().ReachabilityNote();
            Layout();
            break;
        case UiKind::ServerCode:
            serverCode_ = ev->a;
            serverExpiry_ = ev->n;
            Layout();
            break;
        case UiKind::ServerDevices:
            ApplyServerDevices(ev->devices);
            break;
        case UiKind::ServerClient:
            serverClientOnline_ = ev->flag;
            if (ev->flag) {
                serverClient_ = ev->a + "  |  " + ev->b + "  |  " + ev->c;
            } else {
                std::string info = ServerMode::Instance().ClientInfoText();
                serverClient_ = info.empty() ? "No client connected" : info;
            }
            Layout();
            break;
        case UiKind::ClientState:
            clientStatus_ = ev->a;
            Layout();
            break;
        case UiKind::ClientDevices:
            ApplyClientDevices(ev->devices);
            break;
        case UiKind::ClientServer:
            clientServer_ = ev->a + (ev->b.empty() ? "" : ("  |  " + ev->b)) + (ev->c.empty() ? "" : ("  |  " + ev->c));
            Layout();
            break;
        case UiKind::Approval:
            ShowApprovalDialog(ev->approval);
            break;
        case UiKind::Banner:
            bannerText_ = ev->a;
            bannerSeverity_ = ev->b.empty() ? "info" : ev->b;
            bannerUntil_ = NowUnix() + 12;
            break;
        case UiKind::LogTick:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowApprovalDialog(std::shared_ptr<ApprovalRequest> request) {
    ShowApprovalWindow(hwnd_, request);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowSettingsDialog() {
    bool saved = ShowSettings(hwnd_);
    if (saved) {
        Settings& s = GetSettings();
        Logger::Instance().SetMinLevel(static_cast<LogLevel>(s.logLevel));
        SetBanner("Settings saved. Server/client changes apply the next time the mode is started.", "info");
        if (mode_ == Mode::Server && ServerMode::Instance().Running()) {
            serverCode_ = ServerMode::Instance().CodeText();
            serverEndpoint_ = ServerMode::Instance().EndpointText();
            serverNote_ = ServerMode::Instance().ReachabilityNote();
        }
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::ShowInfoDialog(const std::wstring& title, const std::wstring& text) {
    ShowInfoBox(hwnd_, title.c_str(), text, false);
}

}  // namespace mub

#endif  // _WIN32
