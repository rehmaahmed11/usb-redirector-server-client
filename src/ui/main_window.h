// Mini USB Bridge - main window
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../core/protocol.h"
#include "theme.h"
#include "ui_event.h"

namespace mub {

class MainWindow {
public:
    static MainWindow& Instance();

    bool Create(HINSTANCE instance, int showCommand);
    HWND Handle() const { return hwnd_; }
    void OnUiEvent(std::shared_ptr<UiEvent> ev);

private:
    MainWindow() = default;
    ~MainWindow() = default;

    enum class Mode { Choose, Server, Client };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool RegisterWindowClass(HINSTANCE instance);
    void CreateChildControls();
    void BackendCheck();
    void BuildButtons();
    void Layout();
    void Paint(HDC dc);
    void PaintHeader(HDC dc, RECT rc);
    void PaintChoose(HDC dc, RECT content);
    void PaintServer(HDC dc, RECT content);
    void PaintClient(HDC dc, RECT content);
    void PaintBanner(HDC dc, RECT content);
    void PaintLogStrip(HDC dc, RECT content);
    void PaintButton(HDC dc, const HitButton& b);
    void UpdateHover(POINT pt);
    void OnCommand(int id);
    void SwitchMode(Mode mode);
    void ApplyServerDevices(const std::vector<DeviceEntry>& devices);
    void ApplyClientDevices(const std::vector<DeviceEntry>& devices);
    void ShowApprovalDialog(std::shared_ptr<ApprovalRequest> request);
    void ShowSettingsDialog();
    void ShowInfoDialog(const std::wstring& title, const std::wstring& text);
    std::string ClipboardText() const;
    void SetClipboardText(const std::string& text) const;
    void RunAsync(std::function<void()> fn);
    void SetBanner(const std::string& text, const std::string& severity);
    void RefreshStatusPill();
    RECT ContentRect() const;
    int SelectedServerRow() const { return serverSelection_; }

    HINSTANCE instance_ = nullptr;
    HDEVNOTIFY deviceNotify_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND codeEdit_ = nullptr;
    HFONT fontBase_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontTitle_ = nullptr;
    HFONT fontMono_ = nullptr;
    HFONT fontBig_ = nullptr;

    Mode mode_ = Mode::Choose;
    std::vector<HitButton> buttons_;
    int hotButton_ = -1;
    int pressedButton_ = -1;
    int dpi_ = 96;

    // server view state
    std::string serverStatus_ = "Stopped";
    std::string serverCode_;
    uint64_t serverExpiry_ = 0;
    std::string serverClient_ = "No client connected";
    std::string serverEndpoint_ = "-";
    bool serverClientOnline_ = false;
    std::vector<DeviceEntry> serverDevices_;
    std::vector<UsbDeviceRow> serverRows_;
    int serverSelection_ = -1;
    int serverScroll_ = 0;

    // client view state
    std::string clientStatus_ = "Not connected";
    std::string clientServer_ = "";
    std::vector<DeviceEntry> clientDevices_;
    std::vector<UsbDeviceRow> clientRows_;
    std::map<std::string, std::pair<bool, bool>> clientAttach_;  // busid -> (attached, busy)
    int clientSelection_ = -1;
    int clientScroll_ = 0;

    // Backend status shown on the start screen.
    std::string backendServerText_ = "Checking usbipd-win...";
    std::string backendClientText_ = "Checking usbip-win2...";
    bool backendServerOk_ = false;
    bool backendClientOk_ = false;

    std::mutex mu_;
    std::string bannerText_;
    std::string bannerSeverity_ = "info";
    uint64_t bannerUntil_ = 0;

    HWND childEdit_ = nullptr;
    bool clientInputVisible_ = false;
    std::map<int, std::string> rowActions_;
    LayoutRects layout_{};

    friend class SettingsDialog;
};

}  // namespace mub
