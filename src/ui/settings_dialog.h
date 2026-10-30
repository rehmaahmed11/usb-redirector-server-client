// Mini USB Bridge - settings dialog
// SPDX-License-Identifier: MIT
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

#include <functional>
#include <memory>
#include <string>

namespace mub {

// Modal settings window. Returns true when the user saved.
bool ShowSettings(HWND parent);

// Small information / error box with a native look.
void ShowInfoBox(HWND parent, const wchar_t* title, const std::wstring& text, bool error = false);

struct ApprovalRequest;
// Modal "a client is requesting access" dialog with a 60 s auto-reject timer.
void ShowApprovalWindow(HWND parent, std::shared_ptr<ApprovalRequest> request);

}  // namespace mub
