// ============================================================================
// FSlateStandaloneApp — see include/ZSlate/App/SlateStandaloneApp.h
// ============================================================================

#include "ZSlate/App/SlateStandaloneApp.h"

#if defined(_WIN32)

#include "ZSlate/Core/SlateLog.h"
#include "ZSlate/Platform/Win32/SlateWin32Dpi.h"

#include <chrono>
#include <cstdio>

#include <shellapi.h>

namespace ZSlate
{
namespace
{
// Window classes are process-wide, so two apps in one process (a tool hosting
// both a main window and an inspector) must not collide.
unsigned NextClassId()
{
    static unsigned counter = 0;
    return ++counter;
}
}  // namespace

FSlateStandaloneApp::FSlateStandaloneApp() = default;

FSlateStandaloneApp::~FSlateStandaloneApp()
{
    Shutdown();
}

// ---- Lifecycle ------------------------------------------------------------

bool FSlateStandaloneApp::Initialize(const FStandaloneAppDesc& desc)
{
    if (m_Initialized)
        return true;
    m_Desc = desc;

    // Before the first window exists, or the process gets DPI virtualisation
    // instead of real per-monitor scaling.
    Win32::DeclareDpiAwareness();

    // Fonts first: a failure here is survivable (text degrades to blocky), so
    // it is reported rather than fatal.
    if (Win32::LoadSystemFonts(m_FontAtlas, m_Desc.Font))
    {
        m_Batch.SetFontAtlas(&m_FontAtlas);
    }
    else
    {
        Log(ELogLevel::Warning, "[ZSlate] no usable system font found; text will be blocky");
        m_Batch.SetFontAtlas(nullptr);
    }

    m_Scaled.SetPixelSnap(m_Desc.PixelSnap);
    m_LogicalSize = Vector2(m_Desc.Width, m_Desc.Height);

    SetPlatform(this);

    if (m_Desc.InstallTextMeasurer)
        SlateApplication::Get().SetTextMeasurer(&m_Measurer);
    if (m_Desc.InstallClipboard)
        Win32::InstallClipboardBridge();

    // ---- Window class -----------------------------------------------------
    m_ClassName = "ZSlateStandaloneApp_" + std::to_string(NextClassId());

    const HINSTANCE instance = GetModuleHandle(nullptr);
    WNDCLASSEXA wc {};
    wc.cbSize      = sizeof(wc);
    wc.lpfnWndProc = &FSlateStandaloneApp::WndProcThunk;
    wc.hInstance   = instance;
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = m_ClassName.c_str();
    // Only the EX class carries hIconSm, which is what the taskbar shows.
    // LoadImage with an explicit size picks the closest of the .ico's embedded
    // sizes instead of stretching one of them.
    if (m_Desc.LargeIconResId != 0)
        wc.hIcon = static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(m_Desc.LargeIconResId),
                                                 IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (m_Desc.SmallIconResId != 0)
        wc.hIconSm = static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(m_Desc.SmallIconResId),
                                                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (RegisterClassExA(&wc) == 0)
    {
        std::fprintf(stderr, "[ZSlate] RegisterClassEx failed\n");
        return false;
    }

    // ---- Window -----------------------------------------------------------
    // Created at the logical size scaled by the primary monitor, so the UI
    // keeps its intended physical footprint once virtualisation is off.
    const float initial_scale = Win32::QuerySystemDpiScale();
    m_Hwnd = CreateWindowExA(0, m_ClassName.c_str(), m_Desc.Title.c_str(),
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             static_cast<int>(m_Desc.Width * initial_scale),
                             static_cast<int>(m_Desc.Height * initial_scale),
                             nullptr, nullptr, instance, this);
    if (m_Hwnd == nullptr)
    {
        std::fprintf(stderr, "[ZSlate] CreateWindowEx failed\n");
        return false;
    }

    // Per-window DPI is authoritative (multi-monitor); re-derive the logical
    // size from the real client rect now that the window exists.
    const float scale = Win32::QueryDpiScaleForWindow(m_Hwnd);
    Win32::ResizeWindowForDpi(m_Hwnd, static_cast<int>(m_Desc.Width), static_cast<int>(m_Desc.Height),
                              static_cast<unsigned>(scale * 96.0f));
    {
        RECT client {};
        GetClientRect(m_Hwnd, &client);
        ApplyDpi(scale, client.right - client.left, client.bottom - client.top);
    }

    if (m_Desc.AcceptFileDrops)
        DragAcceptFiles(m_Hwnd, TRUE);

    // ---- D3D11 backend ----------------------------------------------------
    m_Backend = std::make_unique<ZSlateD3D11Renderer>(m_Hwnd);
    if (!m_Backend->Init())
    {
        std::fprintf(stderr, "[ZSlate] D3D11 init failed\n");
        m_Backend.reset();
        return false;
    }

    m_Initialized = true;
    return true;
}

void FSlateStandaloneApp::Shutdown()
{
    if (!m_Initialized && m_Hwnd == nullptr && m_Backend == nullptr)
        return;

    SlateApplication::Get().SetTextMeasurer(nullptr);
    SlateApplication::Get().SetRootContent(nullptr);
    SetPlatform(nullptr);

    m_Backend.reset();  // destructor calls Shutdown(); do not call it twice
    if (m_Hwnd != nullptr)
    {
        DestroyWindow(m_Hwnd);
        m_Hwnd = nullptr;
    }
    m_Initialized = false;
    m_Running = false;
}

int FSlateStandaloneApp::Run()
{
    if (m_Hwnd == nullptr)
        return 1;

    ShowWindow(m_Hwnd, m_Desc.ShowCommand);

    m_Running = true;
    auto last = std::chrono::steady_clock::now();
    MSG msg {};
    while (m_Running)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!m_Running)
            break;

        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - last).count();
        last = now;

        if (OnFrame)
            OnFrame();

        SlateApplication::Get().Tick(delta);
        TickFrame();
        Present();

        const unsigned sleep_ms = m_Minimized ? m_Desc.MinimizedSleepMs : m_Desc.IdleSleepMs;
        if (sleep_ms != 0)
            Sleep(sleep_ms);
    }
    return 0;
}

void FSlateStandaloneApp::RequestExit()
{
    m_Running = false;
    if (m_Hwnd != nullptr)
        PostMessageA(m_Hwnd, WM_CLOSE, 0, 0);
}

// ---- Content / frame ------------------------------------------------------

void FSlateStandaloneApp::SetRootContent(const std::shared_ptr<SWidget>& root)
{
    SlateApplication::Get().SetRootContent(root);
}

void FSlateStandaloneApp::TickFrame()
{
    auto root = SlateApplication::Get().GetInputRoot();
    if (root)
    {
        const bool over = m_MousePos.x >= 0.0f && m_MousePos.y >= 0.0f &&
                          m_MousePos.x < m_LogicalSize.x && m_MousePos.y < m_LogicalSize.y;
        m_Input.ProcessMouse(root, m_MousePos, over, m_MouseDown[0], m_PendingWheel, m_MouseDown[1]);
    }
    m_PendingWheel = 0.0f;  // consumed

    ISlateRenderer* renderer = GetPaintRenderer();
    renderer->BeginFrame();
    const UIRect viewport(0.0f, 0.0f, m_LogicalSize.x, m_LogicalSize.y);
    if (m_Desc.ClearBeforePaint)
        renderer->DrawQuad(viewport, m_Desc.ClearColor);
    SlateApplication::Get().PaintInto(renderer, viewport);
    if (OnPostPaint)
        OnPostPaint(renderer, viewport);
    renderer->EndFrame();
}

void FSlateStandaloneApp::Present()
{
    if (m_Backend == nullptr || m_Minimized || m_Hwnd == nullptr || !IsWindow(m_Hwnd))
        return;
    m_Backend->Render(m_Batch, m_FontAtlas.IsLoaded() ? &m_FontAtlas : nullptr);
}

// ---- Misc accessors -------------------------------------------------------

void FSlateStandaloneApp::SetWindowTitle(const std::string& title)
{
    if (m_Hwnd != nullptr)
        SetWindowTextA(m_Hwnd, title.c_str());
}

void FSlateStandaloneApp::SetRendererOverride(ISlateRenderer* renderer)
{
    m_RendererOverride = renderer;
}

ISlateRenderer* FSlateStandaloneApp::GetPaintRenderer()
{
    return (m_RendererOverride != nullptr) ? m_RendererOverride
                                          : static_cast<ISlateRenderer*>(&m_Scaled);
}

ISlateRenderer* FSlateStandaloneApp::GetRenderer()
{
    return GetPaintRenderer();
}

void FSlateStandaloneApp::ApplyDpi(float scale, int physical_w, int physical_h)
{
    const float s = (scale > 0.0f) ? scale : 1.0f;
    m_DpiScale = s;
    m_Scaled.SetScale(s);
    m_LogicalSize = Vector2(static_cast<float>(physical_w) / s, static_cast<float>(physical_h) / s);
}

// ---- ISlatePlatform -------------------------------------------------------

bool FSlateStandaloneApp::IsMouseButtonDown(int button) const
{
    return (button >= 0 && button < 3) ? m_MouseDown[button] : false;
}

bool FSlateStandaloneApp::IsKeyDown(int key) const
{
    return (GetKeyState(key) & 0x8000) != 0;
}

float FSlateStandaloneApp::GetTimeSeconds() const
{
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

// ---- Win32 plumbing -------------------------------------------------------

LRESULT CALLBACK FSlateStandaloneApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // WM_NCCREATE arrives before WM_CREATE, so routing through it means no
    // message is ever delivered with a null app pointer.
    if (msg == WM_NCCREATE)
    {
        auto* app = static_cast<FSlateStandaloneApp*>(
            reinterpret_cast<CREATESTRUCTA*>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app != nullptr && app->m_Hwnd == nullptr)
            app->m_Hwnd = hwnd;
    }

    auto* app = reinterpret_cast<FSlateStandaloneApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (app == nullptr)
        return DefWindowProcA(hwnd, msg, wp, lp);

    return app->HandleMessage(hwnd, msg, wp, lp);
}

LRESULT FSlateStandaloneApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        m_Minimized = (wp == SIZE_MINIMIZED);
        if (!m_Minimized && LOWORD(lp) != 0 && HIWORD(lp) != 0)
        {
            // lp is physical: the swapchain wants those, the widget tree logical.
            ApplyDpi(m_DpiScale, LOWORD(lp), HIWORD(lp));
            if (m_Backend)
                m_Backend->Resize(LOWORD(lp), HIWORD(lp));
            if (OnResize)
                OnResize(static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp)));
        }
        return 0;

    case WM_DPICHANGED:
    {
        // Dragged to a monitor with a different scale factor. Take the rect
        // Windows suggests, then re-derive the logical size from it; the glyphs
        // re-bake at the new pixel size automatically.
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested != nullptr)
        {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT client {};
        GetClientRect(hwnd, &client);
        // LOWORD = new DPI on the X axis (Y is in HIWORD; identical for the
        // uniform scaling Windows applies).
        ApplyDpi(static_cast<float>(LOWORD(wp)) / 96.0f,
                 client.right - client.left, client.bottom - client.top);
        if (m_Backend)
            m_Backend->Resize(static_cast<uint32_t>(client.right - client.left),
                              static_cast<uint32_t>(client.bottom - client.top));
        if (OnDpiChanged)
            OnDpiChanged(m_DpiScale);
        return 0;
    }

    case WM_MOUSEMOVE:
        m_MousePos = ToLogical(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        return 0;

    case WM_MOUSELEAVE:
        m_MousePos = Vector2(-1.0f, -1.0f);
        return 0;

    case WM_LBUTTONDOWN:
        m_MousePos = ToLogical(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        m_MouseDown[0] = true;
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        m_MouseDown[0] = false;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        m_MouseDown[1] = true;
        return 0;
    case WM_RBUTTONUP:
        m_MouseDown[1] = false;
        return 0;
    case WM_MBUTTONDOWN:
        m_MouseDown[2] = true;
        return 0;
    case WM_MBUTTONUP:
        m_MouseDown[2] = false;
        return 0;

    case WM_MOUSEWHEEL:
    {
        // Wheel coordinates are screen-space; put the pointer where the wheel
        // actually happened before routing the notches.
        POINT pos {static_cast<LONG>(static_cast<short>(LOWORD(lp))),
                   static_cast<LONG>(static_cast<short>(HIWORD(lp)))};
        ScreenToClient(hwnd, &pos);
        m_MousePos = ToLogical(static_cast<short>(pos.x), static_cast<short>(pos.y));
        // Accumulate: several wheel messages can arrive between frames.
        m_PendingWheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
        if (OnWheel)
            OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA));
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (msg == WM_KEYDOWN)
        {
            SlateInputRouter::SetModifierState((GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                               (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                               (GetKeyState(VK_MENU) & 0x8000) != 0);
        }
        if (OnKey && OnKey(static_cast<unsigned>(wp), msg == WM_KEYDOWN))
            return 0;
        if (msg == WM_KEYDOWN)
        {
            const EKey key = Win32::KeyFromVirtualKey(static_cast<unsigned>(wp));
            if (key != EKey::Unknown)
                m_Input.ProcessKey(key);
        }
        return 0;

    case WM_CHAR:
    {
        // WM_CHAR carries the composed character (including CJK input) and is
        // the only way text reaches the editable widgets. Control characters
        // (Enter, Backspace, Ctrl+letter) arrive as keys instead.
        const unsigned unit = static_cast<unsigned>(wp);
        if (unit < 32 || unit == 127)
            return 0;
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            m_PendingHighSurrogate = unit;  // first half of a surrogate pair
            return 0;
        }
        unsigned codepoint = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            if (m_PendingHighSurrogate == 0)
                return 0;
            codepoint = 0x10000u + ((m_PendingHighSurrogate - 0xD800u) << 10) + (unit - 0xDC00u);
        }
        m_PendingHighSurrogate = 0;
        if (OnChar && OnChar(codepoint))
            return 0;
        m_Input.ProcessChar(codepoint);
        return 0;
    }

    case WM_DROPFILES:
    {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        char path[MAX_PATH] {};
        if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0)
        {
            if (!OnDropFile || !OnDropFile(std::string(path)))
            {
                // Unhandled: nothing to do -- the app did not opt into drops.
            }
        }
        DragFinish(drop);
        SetForegroundWindow(hwnd);
        return 0;
    }

    // Focus loss with a button held would otherwise leave the widget tree
    // stuck in a pressed state until the next click.
    case WM_CAPTURECHANGED:
        if (!m_MouseDown[0])
            return 0;
        [[fallthrough]];
    case WM_KILLFOCUS:
        m_MouseDown[0] = m_MouseDown[1] = m_MouseDown[2] = false;
        m_Input.ResetPointerState();
        return 0;

    case WM_CLOSE:
        if (OnClose && !OnClose())
            return 0;  // host vetoed (e.g. work in flight)
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

Vector2 FSlateStandaloneApp::ToLogical(int physical_x, int physical_y) const
{
    const float inv = 1.0f / m_DpiScale;
    return Vector2(static_cast<float>(physical_x) * inv, static_cast<float>(physical_y) * inv);
}

}  // namespace ZSlate

#endif  // _WIN32
