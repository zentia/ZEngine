// =============================================================================
// TexPreview — standalone tool, ZSlate + D3D11, no ZRuntime
// =============================================================================

// PCH may include windows.h which #defines DrawText→DrawTextA.  Undefine it
// BEFORE any ZSlate header is parsed, otherwise ISlateRenderer::DrawText
// becomes ISlateRenderer::DrawTextA and ZSlateBatchedRenderer fails to
// instantiate (abstract class error).
#ifdef DrawText
#undef DrawText
#endif

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "TexPreviewWindow.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdio>
#include <filesystem>

// ---- Platform stub (ZSlate requires ISlatePlatform) -------------------------
struct TexPlatform : public ZSlate::ISlatePlatform
{
    ZSlate::ZSlateBatchedRenderer Renderer;
    ZSlate::Vector2 m_MousePos {}, m_WinSize {1024, 768};
    bool m_MouseDown[3] {};

    ZSlate::ISlateRenderer*    GetRenderer()    override { return &Renderer; }
    ZSlate::ISlateFontService* GetFontService() override { return nullptr; }
    ZSlate::Vector2 GetMousePosition()   const override { return m_MousePos; }
    bool IsMouseButtonDown(int b) const override { return b<3?m_MouseDown[b]:false; }
    bool IsKeyDown(int)           const override { return false; }
    float GetTimeSeconds()        const override { return 0.0f; }
    ZSlate::Vector2 GetWindowSize() const override { return m_WinSize; }
};

// ---- App --------------------------------------------------------------------
struct TexApp
{
    TexPlatform                      m_Platform;
    ZSlate::SlateInputRouter         m_Input;
    std::shared_ptr<TexPreviewWindow> m_Window;

    void BuildUI(const char* path)
    {
        m_Window = std::make_shared<TexPreviewWindow>();
        m_Window->Visibility = ZSlate::EVisibility::Visible;
        if (path && path[0])
            m_Window->SetTexture(std::filesystem::path(path));
        ZSlate::SlateApplication::Get().SetRootContent(m_Window);
    }

    void SetD3D11Device(ID3D11Device* d, ID3D11DeviceContext* c)
    {
#ifdef _WIN32
        if (m_Window) m_Window->SetD3D11Device(d, c);
#endif
    }

    void RunFrame()
    {
        auto root = ZSlate::SlateApplication::Get().GetRootContent();
        if (root) {
            bool over = m_Platform.m_MousePos.x >= 0;
            m_Input.ProcessMouse(root, m_Platform.m_MousePos, over,
                m_Platform.m_MouseDown[0], 0.0f, m_Platform.m_MouseDown[1]);
        }
        m_Platform.Renderer.BeginFrame();
        ZSlate::UIRect r(0, 0, m_Platform.GetWindowSize().x, m_Platform.GetWindowSize().y);
        ZSlate::SlateApplication::Get().PaintInto(&m_Platform.Renderer, r);
        m_Platform.Renderer.EndFrame();
    }
};

// ---- WinMain ----------------------------------------------------------------
#ifdef _WIN32

struct WinCtx { TexApp* app; ZSlate::ZSlateD3D11Renderer* backend; };

LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    WinCtx* ctx = nullptr;
    if (msg == WM_CREATE) {
        ctx = (WinCtx*)((CREATESTRUCT*)lp)->lpCreateParams;
        SetWindowLongPtr(hw, GWLP_USERDATA, (LONG_PTR)ctx);
    } else ctx = (WinCtx*)GetWindowLongPtr(hw, GWLP_USERDATA);

    TexApp* app = ctx ? ctx->app : nullptr;
    switch (msg) {
    case WM_SIZE:
        if (app) { app->m_Platform.m_WinSize = ZSlate::Vector2((float)LOWORD(lp),(float)HIWORD(lp));
                   if (ctx->backend) ctx->backend->Resize(LOWORD(lp), HIWORD(lp)); } return 0;
    case WM_MOUSEMOVE:   if (app) app->m_Platform.m_MousePos=ZSlate::Vector2((float)LOWORD(lp),(float)HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: if (app) app->m_Platform.m_MouseDown[0]=true;  return 0;
    case WM_LBUTTONUP:   if (app) app->m_Platform.m_MouseDown[0]=false; return 0;
    case WM_RBUTTONDOWN: if (app) app->m_Platform.m_MouseDown[1]=true;  return 0;
    case WM_RBUTTONUP:   if (app) app->m_Platform.m_MouseDown[1]=false; return 0;
    case WM_MBUTTONDOWN: if (app) app->m_Platform.m_MouseDown[2]=true;  return 0;
    case WM_MBUTTONUP:   if (app) app->m_Platform.m_MouseDown[2]=false; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hw, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const char* path = nullptr;
    std::string pathStr;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2) {
            int sz = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
            pathStr.resize(sz > 0 ? sz - 1 : 0);
            if (sz > 0) WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, &pathStr[0], sz, nullptr, nullptr);
            path = pathStr.c_str();
        }
        if (argv) LocalFree(argv);
    }

    printf("=== TexPreview (ZSlate + D3D11) ===\n");

    // Load font atlas for info overlay text
    ZSlate::ZSlateFontAtlas fontAtlas;
    if (fontAtlas.LoadFromFile("C:\\Windows\\Fonts\\segoeui.ttf"))
    {
        fontAtlas.AddFallbackFont("C:\\Windows\\Fonts\\msyh.ttc");  // CJK
        std::printf("[TexPreview] Font atlas loaded (Segoe UI + CJK fallback)\n");
    }
    else
        std::fprintf(stderr, "[TexPreview] WARNING: font atlas load failed, text will be blocky\n");

    TexApp app;
    ZSlate::SetPlatform(&app.m_Platform);
    app.m_Platform.Renderer.SetFontAtlas(&fontAtlas);
    app.BuildUI(path);

    WNDCLASSA wc {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "TexPreview";
    RegisterClassA(&wc);

    WinCtx ctx {&app, nullptr};
    HWND hw = CreateWindowA("TexPreview", "Texture Preview (ZSlate)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        (int)app.m_Platform.m_WinSize.x, (int)app.m_Platform.m_WinSize.y,
        nullptr, nullptr, GetModuleHandle(nullptr), &ctx);

    auto* backend = new ZSlate::ZSlateD3D11Renderer(hw);
    ctx.backend = backend;

    if (!backend->Init()) { delete backend; return 1; }
    // Give TexPreviewWindow access to the D3D11 device so it can create GPU textures
    app.SetD3D11Device(backend->GetDevice(), backend->GetContext());
    ShowWindow(hw, SW_SHOW);

    MSG msg {};
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto out;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        app.RunFrame();
        backend->Render(app.m_Platform.Renderer, &fontAtlas);
        Sleep(1);
    }
out:
    backend->Shutdown();
    delete backend;
    return 0;
}

#else
int main() { printf("TexPreview: Windows + D3D11 only\n"); return 1; }
#endif
