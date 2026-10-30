// Mini USB Bridge - visual theme + owner-draw widget helpers
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace mub {

// Flat palette (light, high contrast, no gradients).
namespace theme {
constexpr COLORREF kBg = RGB(0xF4, 0xF6, 0xF9);
constexpr COLORREF kCard = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kCardAlt = RGB(0xF8, 0xFA, 0xFC);
constexpr COLORREF kBorder = RGB(0xE2, 0xE8, 0xF0);
constexpr COLORREF kHeader = RGB(0x16, 0x21, 0x39);
constexpr COLORREF kHeaderSub = RGB(0x94, 0xA3, 0xB8);
constexpr COLORREF kText = RGB(0x1E, 0x29, 0x3B);
constexpr COLORREF kTextDim = RGB(0x64, 0x74, 0x8B);
constexpr COLORREF kTextFaint = RGB(0x94, 0xA3, 0xB8);
constexpr COLORREF kAccent = RGB(0x25, 0x63, 0xEB);
constexpr COLORREF kAccentDark = RGB(0x1D, 0x4E, 0xD8);
constexpr COLORREF kAccentSoft = RGB(0xEF, 0xF4, 0xFF);
constexpr COLORREF kSuccess = RGB(0x15, 0x9E, 0x5B);
constexpr COLORREF kSuccessSoft = RGB(0xEC, 0xFD, 0xF5);
constexpr COLORREF kWarn = RGB(0xD9, 0x77, 0x06);
constexpr COLORREF kWarnSoft = RGB(0xFF, 0xFB, 0xEB);
constexpr COLORREF kError = RGB(0xDC, 0x26, 0x26);
constexpr COLORREF kErrorSoft = RGB(0xFE, 0xF2, 0xF2);
constexpr COLORREF kMono = RGB(0x0F, 0x17, 0x2A);
}  // namespace theme

enum class ButtonStyle {
    Primary,
    Secondary,
    Danger,
    Ghost,
    RowAction,
    RowActionOn,
};

struct HitButton {
    RECT rc{};
    int id = 0;
    std::wstring label;
    ButtonStyle style = ButtonStyle::Secondary;
    bool enabled = true;
    bool hover = false;
    int radius = 6;
};

// A device row as rendered by the list (server or client view).
struct UsbDeviceRow {
    std::string busid;
    std::string name;
    std::string manufacturer;
    std::string vidpid;
    std::string status;
    bool shared = false;
    bool attached = false;
    bool present = true;
    uint16_t vid = 0;
    uint16_t pid = 0;
};

// Geometry computed once per layout pass; both painting and hit testing use it.
struct LayoutRects {
    RECT header{};
    RECT content{};
    RECT back{};
    RECT banner{};
    RECT cardServer{};
    RECT cardClient{};
    RECT codeBox{};
    RECT copyBtn{};
    RECT regenBtn{};
    RECT disconnectBtn{};
    RECT devicesCard{};
    RECT listArea{};
    RECT shareBtn{};
    RECT stopShareBtn{};
    RECT shareAllBtn{};
    RECT stopAllBtn{};
    RECT logStrip{};
    RECT openLogBtn{};
    RECT inputBox{};
    RECT pasteBtn{};
    RECT connectBtn{};
    RECT connectCard{};
    RECT clientDevicesCard{};
    RECT refreshBtn{};
    RECT clientDisconnectBtn{};
    RECT settingsBtn{};
    RECT chooseServerCard{};
    RECT chooseClientCard{};
    RECT backendCard{};
    int rowHeight = 40;
    int visibleRows = 5;
};

namespace ui {
void FillRectColor(HDC dc, const RECT& rc, COLORREF color);
void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF color);
void FrameRoundRect(HDC dc, const RECT& rc, int radius, COLORREF color, int width = 1);
void DrawTextIn(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
                UINT flags = DT_LEFT | DT_SINGLELINE);
void DrawTextEllipsis(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color, UINT flags);
int TextWidth(HDC dc, const std::wstring& text, HFONT font);
void DrawStatusDot(HDC dc, int x, int y, int radius, COLORREF color);
void DrawBadge(HDC dc, RECT rc, const std::wstring& text, HFONT font, COLORREF bg, COLORREF fg);
}  // namespace ui

}  // namespace mub
