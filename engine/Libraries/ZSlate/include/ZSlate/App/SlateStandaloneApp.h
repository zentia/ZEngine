#pragma once
// ============================================================================
// FSlateStandaloneApp — the Win32 + D3D11 host every standalone ZSlate tool
//                       was re-implementing by hand
// ============================================================================
// Five executables (LuaMemPreview, TexPreview, ClientProjBranches,
// ZEnvironmentCollector, and the ZGpuSelectionInspector that is now a tab inside
// ZEnvironmentCollector) each carried their own copy
// of: an ISlatePlatform subclass, a WNDCLASS/CreateWindow/PeekMessage host, a
// DPI declaration + WM_DPICHANGED handler, and a hardcoded font-atlas load.
// The copies had drifted — two had no DPI support at all, one derived the
// Fonts directory correctly, the DPI decorator was constructed with different
// pixel-snapping flags — which is why "every tool needs its own DPI and font
// work" was true. This class is the single implementation.
//
// What it owns, in the order a frame runs:
//   1. DPI awareness (declared before the first window exists).
//   2. Font atlas: system face + CJK fallback, via Win32::LoadSystemFonts().
//   3. The window, the D3D11 backend and the message pump.
//   4. ZSlateBatchedRenderer (physical pixels) wrapped in
//      FSlateDpiScaleRenderer (logical -> physical), so the widget tree keeps
//      authoring in 96-DPI units.
//   5. SlateInputRouter, fed with coordinates already divided by the DPI scale.
//
// Coordinate contract, stated once: Win32 messages and the swapchain are
// physical; the widget tree is logical. This class is the only place the two
// meet — the WndProc divides on the way in, the decorator multiplies on the
// way out. Nothing outside this class should do the conversion.
//
// Windows-only by construction (it creates an HWND and a D3D11 device), so the
// whole header is compiled out elsewhere, exactly like
// ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h.
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

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Backend/SlateUIRendererBackend.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Platform/Win32/SlateWin32Host.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#include <functional>
#include <memory>
#include <string>

namespace ZSlate
{

struct FStandaloneAppDesc
{
    std::string Title  = "ZSlate";
    // Logical (96-DPI) client size. The window is created at this size scaled
    // by the monitor factor, so the UI keeps the intended physical footprint.
    float Width  = 1024.0f;
    float Height = 768.0f;

    // Lead font face. Mono for stacks / column-aligned tables, Sans otherwise.
    Win32::ESystemFont Font = Win32::ESystemFont::Sans;

    // FSlateDpiScaleRenderer edge snapping. On by default: at fractional
    // scales (125%, 150%) logical integers land between physical pixels and
    // neighbouring boxes pick up 1px seams without it.
    bool PixelSnap = true;

    // Route STextBlock's layout-time measurement through the DPI decorator so
    // text is measured at the size it will actually be rasterized at.
    bool InstallTextMeasurer = true;

    // OS clipboard for SEditableTextBox copy/paste.
    bool InstallClipboard = true;

    bool AcceptFileDrops = false;

    // Window-class icons (resource ids). 0 keeps the system default; only the
    // EX window class carries the small icon the taskbar shows.
    int LargeIconResId = 0;
    int SmallIconResId = 0;

    // Full-viewport fill painted before the widget tree. Widgets that need a
    // transparent background set ClearBeforePaint = false.
    bool    ClearBeforePaint = true;
    UIColor ClearColor {0.086f, 0.086f, 0.098f, 1.0f};

    // Message-pump idle pacing. 0 runs the loop flat out.
    unsigned IdleSleepMs      = 1;
    unsigned MinimizedSleepMs = 50;

    int ShowCommand = SW_SHOW;
};

class FSlateStandaloneApp : public ISlatePlatform
{
public:
    FSlateStandaloneApp();
    ~FSlateStandaloneApp() override;

    FSlateStandaloneApp(const FSlateStandaloneApp&) = delete;
    FSlateStandaloneApp& operator=(const FSlateStandaloneApp&) = delete;

    // ---- Lifecycle -------------------------------------------------------
    // Returns false after printing a diagnostic; the caller turns that into
    // its own exit code (or, for GUI-only tools, an error dialog).
    bool Initialize(const FStandaloneAppDesc& desc);
    void Shutdown();

    // Message pump: TickFrame() + Present() until the window closes.
    int  Run();
    void RequestExit();

    // ---- Content ---------------------------------------------------------
    void SetRootContent(const std::shared_ptr<SWidget>& root);

    // ---- Frame -----------------------------------------------------------
    // Split so a host that owns its own pump can drive them separately.
    void TickFrame();  // input routing + layout + paint into the batched renderer
    void Present();    // D3D11 present (no-op when headless or minimized)

    // ---- Per-frame escape hatches (all optional) --------------------------
    // Returning true from OnChar / OnKey / OnDropFile consumes the event, i.e.
    // ZSlate's input router will not see it.
    std::function<void()>                       OnFrame;      // once per pumped frame
    std::function<bool(unsigned codepoint)>     OnChar;
    std::function<bool(unsigned vk, bool down)> OnKey;
    std::function<bool(const std::string&)>     OnDropFile;   // UTF-8 path
    std::function<void(int phys_w, int phys_h)> OnResize;
    std::function<void(float scale)>            OnDpiChanged;
    std::function<bool()>                       OnClose;      // true = allow the close
    std::function<void(float notches)>          OnWheel;
    // Painted after the widget tree but before EndFrame, for overlays that
    // must escape their slot (a hover card overlapping the panel below).
    std::function<void(ISlateRenderer*, const UIRect&)> OnPostPaint;

    // ---- Accessors --------------------------------------------------------
    HWND                    GetHwnd()     const { return m_Hwnd; }
    ID3D11Device*           GetDevice()         { return m_Backend ? m_Backend->GetDevice() : nullptr; }
    ID3D11DeviceContext*    GetContext()        { return m_Backend ? m_Backend->GetContext() : nullptr; }
    ZSlateBatchedRenderer&  GetBatchedRenderer() { return m_Batch; }
    FSlateDpiScaleRenderer& GetScaledRenderer()  { return m_Scaled; }
    ZSlateFontAtlas&        GetFontAtlas()       { return m_FontAtlas; }
    SlateInputRouter&       GetInputRouter()     { return m_Input; }

    Vector2 GetLogicalSize() const { return m_LogicalSize; }
    bool    IsMouseDown(int button) const { return (button >= 0 && button < 3) && m_MouseDown[button]; }
    void    SetWindowTitle(const std::string& title);

    // Mouse position is mutable because headless verification drives the
    // router with synthetic coordinates (see LuaMemPreview --dump-ui).
    Vector2& MousePosition() { return m_MousePos; }

    // Replace what GetRenderer() hands out. Headless verification swaps in a
    // FSlateRecordingRenderer to assert on the painted tree, optionally
    // wrapped in another decorator to re-paint the same tree at 150%.
    void           SetRendererOverride(ISlateRenderer* renderer);
    // The renderer a caller should paint through this frame: the override when
    // one is installed, otherwise the DPI decorator.
    ISlateRenderer* GetPaintRenderer();

    // Re-derive the logical size from a physical client size. Public so hosts
    // that drive the window themselves (and tests) can force a scale.
    void ApplyDpi(float scale, int physical_w, int physical_h);

    // ---- ISlatePlatform ---------------------------------------------------
    ISlateRenderer* GetRenderer()                  override;
    Vector2 GetMousePosition()               const override { return m_MousePos; }
    bool    IsMouseButtonDown(int button)    const override;
    bool    IsKeyDown(int key)               const override;
    float   GetTimeSeconds()                 const override;
    Vector2 GetWindowSize()                  const override { return m_LogicalSize; }
    float   GetDpiScale()                    const override { return m_DpiScale; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void    RouteMouse();
    Vector2 ToLogical(int physical_x, int physical_y) const;

    FStandaloneAppDesc m_Desc;

    HWND m_Hwnd {nullptr};
    std::string m_ClassName;
    std::unique_ptr<ZSlateD3D11Renderer> m_Backend;

    ZSlateBatchedRenderer  m_Batch;
    FSlateDpiScaleRenderer m_Scaled {&m_Batch, 1.0f, true};
    ZSlateFontAtlas        m_FontAtlas;
    SlateInputRouter       m_Input;
    SlateUIRendererTextMeasurer m_Measurer {&m_Scaled};

    ISlateRenderer* m_RendererOverride {nullptr};

    Vector2 m_MousePos {-1.0f, -1.0f};
    Vector2 m_LogicalSize {1024.0f, 768.0f};
    float   m_DpiScale {1.0f};
    float   m_PendingWheel {0.0f};
    bool    m_MouseDown[3] {};
    // First half of a UTF-16 surrogate pair delivered through WM_CHAR.
    unsigned m_PendingHighSurrogate {0};

    bool m_Minimized {false};
    bool m_Running {false};
    bool m_Initialized {false};
};

}  // namespace ZSlate

#endif  // _WIN32
