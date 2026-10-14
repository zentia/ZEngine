#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <utility>

namespace ClientProj {

// Shared Win32 context menu helper — used by all tabs
inline int ShowContextMenu(HWND hWnd, int x, int y, const std::vector<std::pair<std::string, int>>& items) {
    HMENU hMenu = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].first == "-") {
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        } else {
            AppendMenuA(hMenu, MF_STRING, items[i].second, items[i].first.c_str());
        }
    }
    SetForegroundWindow(hWnd);
    int result = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
    return result;
}

// Shared clipboard helper
inline void CopyTextToClipboard(const std::string& text) {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
}

} // namespace ClientProj
