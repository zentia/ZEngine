#pragma once
// ============================================================================
// SlateWin32Dpi — process DPI declaration + per-window scale queries
// ============================================================================
// Consolidated from the copies that used to live in every standalone ZSlate
// tool (LuaMemPreview, TexPreview, ClientProjBranches, WindowsEnvironmentCheck).
// The rules each copy encoded, and why they belong in one place:
//
//   1. DeclareDpiAwareness() must run BEFORE the first window exists. A process
//      that never declares gets DPI *virtualisation*: it renders at 96 DPI and
//      the compositor bitmap-stretches the window to the monitor scale, which
//      is a bilinear resample and exactly why text looks soft. Declaring turns
//      that off; FSlateDpiScaleRenderer then does the scaling properly.
//   2. The widget tree lays out in logical (96-DPI) units while Win32
//      coordinates and the swapchain stay physical. FSlateDpiScaleRenderer
//      multiplies on the way out, the WndProc divides on the way in. Both
//      halves have to agree on one number, so the producer and the consumer
//      of that number live together.
//   3. SetProcessDpiAwarenessContext (Win10 1703) and GetDpiForWindow (1607)
//      are resolved dynamically — a static import would stop the exe from
//      loading on anything older.
//
// TexPreview and ZGpuSelectionInspector previously had none of this, which is
// precisely the "every tool has its own DPI bugs" problem this file removes:
// adopting the shared shell gives them DPI support for free.
// ============================================================================

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef DrawText  // windows.h #defines DrawText -> DrawTextA/W; clashes with ISlateRenderer

namespace ZSlate
{
namespace Win32
{

// Must be called before the first window is created. Idempotent: a process
// that already declared awareness (itself, or a manifest) keeps the stronger
// context, and calling twice is harmless.
void DeclareDpiAwareness();

// Scale factor for a specific window, 1.0 == 96 DPI. Per-monitor aware where
// the OS supports it; falls back to the window DC's LOGPIXELSX otherwise.
float QueryDpiScaleForWindow(HWND hwnd);

// Scale factor of the primary monitor. Used to size the *initial* window,
// before it exists and therefore before QueryDpiScaleForWindow has a window
// to ask about.
float QuerySystemDpiScale();

// Resize `hwnd` so its client area covers logical_w x logical_h at `dpi`.
// Uses AdjustWindowRectExForDpi where available (Win10 1607+); the fallback
// scales the non-client margins manually, because plain AdjustWindowRect is
// DPI-unaware and under-sized the frame at 125%/150%.
void ResizeWindowForDpi(HWND hwnd, int logical_w, int logical_h, unsigned dpi);

}  // namespace Win32
}  // namespace ZSlate

#endif  // _WIN32
