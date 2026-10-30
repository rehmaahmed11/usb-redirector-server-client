// Mini USB Bridge - owner-draw helpers
// SPDX-License-Identifier: MIT
#include "theme.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mub {
namespace ui {

void FillRectColor(HDC dc, const RECT& rc, COLORREF color) {
#if defined(_WIN32)
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
#else
    (void)dc;
    (void)rc;
    (void)color;
#endif
}

void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF color) {
#if defined(_WIN32)
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
#else
    (void)dc;
    (void)rc;
    (void)radius;
    (void)color;
#endif
}

void FrameRoundRect(HDC dc, const RECT& rc, int radius, COLORREF color, int width) {
#if defined(_WIN32)
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
#else
    (void)dc;
    (void)rc;
    (void)radius;
    (void)color;
    (void)width;
#endif
}

void DrawTextIn(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color, UINT flags) {
#if defined(_WIN32)
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, flags);
    SelectObject(dc, oldFont);
#else
    (void)dc;
    (void)text;
    (void)rc;
    (void)font;
    (void)color;
    (void)flags;
#endif
}

void DrawTextEllipsis(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color, UINT flags) {
    DrawTextIn(dc, text, rc, font, color, flags | DT_END_ELLIPSIS | DT_NOPREFIX);
}

int TextWidth(HDC dc, const std::wstring& text, HFONT font) {
#if defined(_WIN32)
    HGDIOBJ oldFont = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    SelectObject(dc, oldFont);
    return sz.cx;
#else
    return static_cast<int>(text.size()) * 7;
#endif
}

void DrawStatusDot(HDC dc, int x, int y, int radius, COLORREF color) {
#if defined(_WIN32)
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
#else
    (void)dc;
    (void)x;
    (void)y;
    (void)radius;
    (void)color;
#endif
}

void DrawBadge(HDC dc, RECT rc, const std::wstring& text, HFONT font, COLORREF bg, COLORREF fg) {
    FillRoundRect(dc, rc, 4, bg);
    DrawTextIn(dc, text, rc, font, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

}  // namespace ui
}  // namespace mub
