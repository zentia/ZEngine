// ============================================================================
// SlateWin32Dpi — see include/ZSlate/Platform/Win32/SlateWin32Dpi.h
// ============================================================================

#include "ZSlate/Platform/Win32/SlateWin32Dpi.h"

#include <algorithm>

#if defined(_WIN32)

namespace ZSlate
{
namespace Win32
{

void DeclareDpiAwareness()
{
    const HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 != nullptr)
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        const auto set_ctx = reinterpret_cast<SetCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (set_ctx != nullptr &&
            set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))))
        {
            return;
        }
    }
    // Pre-1703 fallback: system-wide awareness. No per-monitor updates, but it
    // still stops the stretch on the primary monitor.
    SetProcessDPIAware();
}

float QueryDpiScaleForWindow(HWND hwnd)
{
    const HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 != nullptr)
    {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        const auto get_dpi = reinterpret_cast<GetDpiFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (get_dpi != nullptr)
        {
            const UINT dpi = get_dpi(hwnd);
            if (dpi != 0)
                return static_cast<float>(dpi) / 96.0f;
        }
    }
    // Fallback: device caps on the window DC.
    float scale = 1.0f;
    const HDC dc = GetDC(hwnd);
    if (dc != nullptr)
    {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0)
            scale = static_cast<float>(dpi) / 96.0f;
        ReleaseDC(hwnd, dc);
    }
    return scale;
}

float QuerySystemDpiScale()
{
    float scale = 1.0f;
    const HDC screen = GetDC(nullptr);
    if (screen != nullptr)
    {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        if (dpi > 0)
            scale = static_cast<float>(dpi) / 96.0f;
        ReleaseDC(nullptr, screen);
    }
    return scale;
}

void ResizeWindowForDpi(HWND hwnd, int logical_w, int logical_h, unsigned dpi)
{
    if (hwnd == nullptr || logical_w <= 0 || logical_h <= 0)
        return;

    if (dpi == 0)
        dpi = 96;

    // Contract (see header): the CLIENT area ends up covering logical_w x
    // logical_h at `dpi` -- that is logical_w * dpi/96 PHYSICAL pixels, so the
    // widget tree (which lays out in logical units and is scaled by
    // FSlateDpiScaleRenderer on the way out) keeps its design extent on every
    // monitor scale. Passing the logical values through as physical pixels
    // silently shrank the layout area to logical/scale on scaled displays,
    // which clipped every Fill-width child (buttons, input boxes) at the right
    // edge. AdjustWindowRectExForDpi only scales the frame, so the client size
    // has to be scaled here, before the frame is added.
    const float scale = static_cast<float>(dpi) / 96.0f;
    const LONG client_w = static_cast<LONG>(logical_w * scale);
    const LONG client_h = static_cast<LONG>(logical_h * scale);

    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    RECT rect {0, 0, client_w, client_h};

    const HMODULE user32 = GetModuleHandleA("user32.dll");
    using AdjustFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjust_ex = reinterpret_cast<AdjustFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi")));

    if (adjust_ex != nullptr)
    {
        adjust_ex(&rect, style, FALSE, 0, dpi);
    }
    else
    {
        // AdjustWindowRect computes margins in the system DPI, so the result
        // has to be grown by the ratio between the window's DPI and 96.
        RECT at96 {0, 0, client_w, client_h};
        AdjustWindowRectEx(&at96, style, FALSE, 0);
        const long margin_w = (at96.right - at96.left) - client_w;
        const long margin_h = (at96.bottom - at96.top) - client_h;
        rect.right  = client_w + static_cast<LONG>(margin_w * scale);
        rect.bottom = client_h + static_cast<LONG>(margin_h * scale);
    }

    // Never bigger than the monitor's work area: a fixed logical design at a
    // high scale can legitimately exceed a small panel, and an unsized-clamp
    // window is unusable. The client shrinks proportionally; the scroll box
    // absorbs the difference.
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE)
    {
        const LONG avail_w = info.rcWork.right - info.rcWork.left;
        const LONG avail_h = info.rcWork.bottom - info.rcWork.top;
        const LONG window_w = rect.right - rect.left;
        const LONG window_h = rect.bottom - rect.top;
        if (window_w > avail_w || window_h > avail_h)
        {
            const float fit = std::min(
                window_w > avail_w ? static_cast<float>(avail_w) / window_w : 1.0f,
                window_h > avail_h ? static_cast<float>(avail_h) / window_h : 1.0f);
            const LONG frame_w = window_w - client_w;
            const LONG frame_h = window_h - client_h;
            rect.right = rect.left + static_cast<LONG>(client_w * fit) + frame_w;
            rect.bottom = rect.top + static_cast<LONG>(client_h * fit) + frame_h;
        }
    }

    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace Win32
}  // namespace ZSlate

#endif  // _WIN32
