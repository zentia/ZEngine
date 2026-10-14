#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <d3d11.h>
#undef DrawText  // windows.h #defines DrawText

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/SlateClipboard.h"
#include "ZSlate/Backend/SlateUIRendererBackend.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"

#include "StackTraceApp.h"
#include "UI/MainApp.h"
#include "UI/Tabs.h"
#include "UI/BranchesTab.h"
#include "UI/GitTab.h"
#include "UI/NotesTab.h"
#include "UI/TipsTab.h"
#include "UI/UrlsTab.h"
#include "UI/SettingsTab.h"

#include <cstdio>
#include <memory>
#include <string>
#include <chrono>

// ============================================================================
// Custom Platform
// ============================================================================

class AppPlatform : public ZSlate::ISlatePlatform
{
public:
    ZSlate::ZSlateBatchedRenderer Renderer;
    ZSlate::SlateInputRouter Input;
    bool          m_MouseDown[3] {};
    ZSlate::Vector2 m_MousePos {0, 0};
    ZSlate::Vector2 m_WinSize  {1024, 768};
    // Wheel deltas arrive in WM_MOUSEWHEEL and are consumed once per frame.
    float         m_PendingWheel = 0.0f;

    ZSlate::ISlateRenderer*    GetRenderer()    override { return &Renderer; }
    ZSlate::Vector2 GetMousePosition()   const override { return m_MousePos; }
    bool IsMouseButtonDown(int b) const override { return b < 3 ? m_MouseDown[b] : false; }
    bool IsKeyDown(int)           const override { return false; }
    float GetTimeSeconds()        const override {
        static auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
    ZSlate::Vector2 GetWindowSize() const override { return m_WinSize; }
};

// ============================================================================
// Win32 Window
// ============================================================================

struct WinCtx
{
    AppPlatform* Platform;
    ZSlate::ZSlateD3D11Renderer* Backend = nullptr;
    StackTraceApp::StackTraceApp* StackTrace = nullptr;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WinCtx* ctx = nullptr;
    if (msg == WM_CREATE) {
        ctx = reinterpret_cast<WinCtx*>(((CREATESTRUCT*)lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    } else {
        ctx = reinterpret_cast<WinCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    AppPlatform* platform = ctx ? ctx->Platform : nullptr;
    auto* stackTrace = ctx ? ctx->StackTrace : nullptr;

    switch (msg)
    {
    case WM_SIZE:
        if (platform) {
            platform->m_WinSize = ZSlate::Vector2((float)LOWORD(lp), (float)HIWORD(lp));
            if (ctx->Backend) ctx->Backend->Resize(LOWORD(lp), HIWORD(lp));
        }
        if (stackTrace) stackTrace->OnResize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_MOUSEMOVE:
        if (platform) platform->m_MousePos = ZSlate::Vector2((float)LOWORD(lp), (float)HIWORD(lp));
        return 0;

    case WM_LBUTTONDOWN: if (platform) platform->m_MouseDown[0] = true;  return 0;
    case WM_LBUTTONUP:   if (platform) platform->m_MouseDown[0] = false; return 0;
    case WM_RBUTTONDOWN: if (platform) platform->m_MouseDown[1] = true;  return 0;
    case WM_RBUTTONUP:   if (platform) platform->m_MouseDown[1] = false; return 0;
    case WM_MBUTTONDOWN: if (platform) platform->m_MouseDown[2] = true;  return 0;
    case WM_MBUTTONUP:   if (platform) platform->m_MouseDown[2] = false; return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (platform) platform->Input.ProcessKey((int)wp, msg == WM_KEYDOWN);
        if (stackTrace) stackTrace->OnKeyboardEvent((int)wp, 0, (msg == WM_KEYDOWN) ? 1 : 0, 0);
        return 0;

    // WM_CHAR carries the composed character (including CJK input) and is the
    // only way text ever reaches the editable widgets.
    case WM_CHAR:
        if (platform && wp >= 32 && wp != 127)
            platform->Input.ProcessCharacter((uint32_t)wp);
        return 0;

    case WM_MOUSEWHEEL:
        if (platform) platform->m_PendingWheel += (float)(GET_WHEEL_DELTA_WPARAM(wp)) / (float)WHEEL_DELTA;
        if (stackTrace) stackTrace->OnScrollEvent(0.0, (double)(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ============================================================================
// Helpers
// ============================================================================

static bool InitFonts(ZSlate::ZSlateFontAtlas& atlas, ZSlate::ZSlateBatchedRenderer& renderer)
{
    const char* fonts[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\consola.ttf",
    };
    for (const char* f : fonts) {
        if (atlas.LoadFromFile(f)) { printf("[Font] Loaded: %s\n", f); break; }
    }
    if (atlas.IsLoaded()) {
        const char* cjk[] = { "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simsun.ttc" };
        for (const char* f : cjk) {
            if (atlas.AddFallbackFont(f)) { printf("[Font] CJK fallback: %s\n", f); break; }
        }
    }
    renderer.SetFontAtlas(atlas.IsLoaded() ? &atlas : nullptr);
    return atlas.IsLoaded();
}

static void RunFrame(AppPlatform& platform)
{
    auto root = ZSlate::SlateApplication::Get().GetRootContent();
    if (root) {
        bool over = platform.m_MousePos.x >= 0 && platform.m_MousePos.y >= 0;
        platform.Input.ProcessMouse(root, platform.m_MousePos, over,
            platform.m_MouseDown[0], platform.m_PendingWheel, platform.m_MouseDown[1]);
    }
    platform.m_PendingWheel = 0.0f;

    platform.Renderer.BeginFrame();

    ZSlate::UIRect region(0, 0, platform.GetWindowSize().x, platform.GetWindowSize().y);
    ZSlate::SlateApplication::Get().PaintInto(&platform.Renderer, region);

    platform.Renderer.EndFrame();
}

// ============================================================================
// Clipboard bridge — ZSlate never touches the OS clipboard itself.
// ============================================================================

static void Win32SetClipboardText(const char* text, void*)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wideLength > 0)
    {
        const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
                                           static_cast<SIZE_T>(wideLength) * sizeof(wchar_t));
        if (memory)
        {
            wchar_t* destination = static_cast<wchar_t*>(GlobalLock(memory));
            if (destination)
            {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, destination, wideLength);
                GlobalUnlock(memory);
            }
            SetClipboardData(CF_UNICODETEXT, memory);
        }
    }
    CloseClipboard();
}

// ============================================================================
// Main Entry Point
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    printf("=== ClientProjBranches.Cpp (D3D11) ===\n\n");
    fflush(stdout);

    // ---- App ----
    printf("Creating app...\n"); fflush(stdout);
    AppPlatform platform;
    ZSlate::SetPlatform(&platform);

    // COM init (required for IFileDialog)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Let the Stack Trace tab copy its output.
    ZSlate::GSlateSetClipboard = &Win32SetClipboardText;

    // ---- Fonts ----
    ZSlate::ZSlateFontAtlas fontAtlas;
    InitFonts(fontAtlas, platform.Renderer);

    // ---- Text measurer ----
    auto measurer = std::make_shared<ZSlate::SlateUIRendererTextMeasurer>(&platform.Renderer);
    ZSlate::SlateApplication::Get().SetTextMeasurer(measurer.get());

    // ---- Build multi-tab UI ----
    auto stackTrace = std::make_shared<StackTraceApp::StackTraceApp>();
    stackTrace->Initialize();

    auto branchesTab = std::make_shared<ClientProj::BranchesTab>();
    auto gitTab      = std::make_shared<ClientProj::GitTab>();
    auto notesTab    = std::make_shared<ClientProj::NotesTab>();
    auto tipsTab     = std::make_shared<ClientProj::TipsTab>();
    auto urlsTab     = std::make_shared<ClientProj::UrlsTab>();
    auto settingsTab = std::make_shared<ClientProj::SettingsTab>();

    ClientProj::MainApp mainApp;
    mainApp.SetInputRouter(&platform.Input);
    mainApp.AddTab("Branches",    branchesTab->GetRoot());
    mainApp.AddTab("Git",         gitTab->GetRoot());
    mainApp.AddTab("Stack Trace", stackTrace->GetRoot());
    mainApp.AddTab("TODO",        notesTab->GetRoot());
    mainApp.AddTab("Tips",        tipsTab->GetRoot());
    mainApp.AddTab("URLs",        urlsTab->GetRoot());
    mainApp.AddTab("Settings",    settingsTab->GetRoot());
    mainApp.SelectTab(0);  // Start on Branches tab
    printf("UI initialized\n"); fflush(stdout);

    // ---- Win32 Window ----
    WNDCLASSA wc {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInstance;
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ClientProjBranchesWnd";
    RegisterClassA(&wc);

    WinCtx ctx {&platform, nullptr, stackTrace.get()};
    HWND hwnd = CreateWindowA("ClientProjBranchesWnd", "ClientProjBranches",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        (int)platform.m_WinSize.x, (int)platform.m_WinSize.y,
        nullptr, nullptr, hInstance, &ctx);

    // ---- D3D11 Backend ----
    auto* backend = new ZSlate::ZSlateD3D11Renderer(hwnd);
    ctx.Backend = backend;

    if (!backend->Init()) {
        printf("D3D11 init failed\n");
        delete backend;
        return 1;
    }
    printf("D3D11 initialized\n"); fflush(stdout);

    ShowWindow(hwnd, nCmdShow);

    // ---- Message Loop ----
    printf("Entering main loop...\n"); fflush(stdout);

    MSG msg {};
    int frameCount = 0;
    while (true)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto cleanup;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Handle deferred UI rebuilds outside of input processing
        branchesTab->OnFrame();

        RunFrame(platform);
        backend->Render(platform.Renderer, &fontAtlas);

        frameCount++;
        if (frameCount % 120 == 0) {
            printf("Frame %d: %zu draw cmds\n", frameCount, platform.Renderer.GetCommands().size());
        }
        Sleep(1);  // ~60 FPS cap
    }

cleanup:
    printf("\nCleaning up...\n"); fflush(stdout);

    stackTrace->Shutdown();
    delete backend;  // destructor calls Shutdown()

    CoUninitialize();

    printf("=== Application Exited ===\n");
    return 0;
}
