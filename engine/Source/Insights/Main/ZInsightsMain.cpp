// ----------------------------------------------------------------------------
// ZInsights.exe -- the standalone ZEngine Insights viewer.
//
// A tiny Win32 + Direct2D host that loads a `.ztrace` capture (written by the
// editor's Insights panel "Save & View" button, the `insights.dump` console
// command, or any future producer) and renders it through the shared
// SInsightsTimeline flame-chart widget -- the exact same widget the in-editor
// panel paints, just over a Direct2D UIRenderer instead of the engine RHI.
//
// Conceptually mirrors Unreal Insights: a separate analyzer process decoupled
// from the running editor. This first version is offline/file-based (the user
// chose the .ztrace path over a live socket); a live TCP transport can be
// layered on later behind the same snapshot model.
//
// Usage:  ZInsights.exe [path\to\capture.ztrace]
//         (or drag a .ztrace onto the window / press O to open / F to fit)
// ----------------------------------------------------------------------------

#include "CommonPCH/pch.h"  // windows.h, std, engine include dirs

#include <commdlg.h>
#include <d2d1.h>
#include <shellapi.h>
#include <windowsx.h>

#include "Insights/Render/D2DUIRenderer.h"
#include "Runtime/Profiler/InsightsTrace.h"
#include "Runtime/Profiler/SInsightsTimeline.h"

#include <filesystem>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr float kStatusBarHeight = 26.0f;
constexpr wchar_t kWindowClass[] = L"ZEngineInsightsWindow";

HWND g_Hwnd = nullptr;
ID2D1Factory* g_D2DFactory = nullptr;
ID2D1HwndRenderTarget* g_RenderTarget = nullptr;
ZInsights::D2DUIRenderer* g_Renderer = nullptr;

ZEngine::Insights::InsightsSnapshot g_Snapshot;
ZSlate::SInsightsTimeline g_Timeline;
std::wstring g_TraceFile;
bool g_FitOnNextPaint = false;

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

void UpdateTitle()
{
    std::wstring title = L"ZEngine Insights";
    if (!g_TraceFile.empty())
        title += L"  -  " + std::filesystem::path(g_TraceFile).filename().wstring();
    if (g_Hwnd)
        SetWindowTextW(g_Hwnd, title.c_str());
}

void LoadTraceFile(const std::wstring& path)
{
    const std::string utf8 = ToUtf8(path);
    ZEngine::Insights::InsightsSnapshot loaded;
    if (!ZEngine::Insights::LoadTrace(utf8, loaded))
    {
        MessageBoxW(g_Hwnd, (L"Failed to load trace:\n" + path).c_str(), L"ZEngine Insights",
                    MB_OK | MB_ICONERROR);
        return;
    }
    g_Snapshot = std::move(loaded);
    g_Timeline.SetSnapshot(&g_Snapshot);
    g_TraceFile = path;
    g_FitOnNextPaint = true;
    UpdateTitle();
    if (g_Hwnd)
        InvalidateRect(g_Hwnd, nullptr, FALSE);
}

void OpenFileDialog()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_Hwnd;
    ofn.lpstrFilter = L"ZEngine Insights Trace (*.ztrace)\0*.ztrace\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open .ztrace capture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        LoadTraceFile(file);
}

void CreateRenderTarget()
{
    if (g_D2DFactory == nullptr || g_Hwnd == nullptr)
        return;
    if (g_RenderTarget)
    {
        g_RenderTarget->Release();
        g_RenderTarget = nullptr;
    }

    RECT rc {};
    GetClientRect(g_Hwnd, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(rc.right - rc.left, 1));
    const UINT h = static_cast<UINT>(std::max<LONG>(rc.bottom - rc.top, 1));

    // Force 96 DPI so 1 DIP == 1 pixel: mouse coords (pixels) then align exactly
    // with the widget geometry we hand the timeline.
    const D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
        D2D1::HwndRenderTargetProperties(g_Hwnd, D2D1::SizeU(w, h));

    if (SUCCEEDED(g_D2DFactory->CreateHwndRenderTarget(rt_props, hwnd_props, &g_RenderTarget)))
    {
        if (g_Renderer)
            g_Renderer->SetRenderTarget(g_RenderTarget);
    }
}

std::string BuildStatusText()
{
    char buf[256];
    if (g_Snapshot.tracks.empty())
    {
        std::snprintf(buf, sizeof(buf),
                      "No trace loaded.  [O] open  -  or drag a .ztrace onto the window");
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%zu threads  |  %zu frames    [O]pen  [F]it  scroll=zoom  drag=pan",
                  g_Snapshot.tracks.size(), g_Snapshot.frame_starts.size());
    std::string status = buf;
    if (!g_Timeline.HoverText().empty())
        status += "      " + g_Timeline.HoverText();
    return status;
}

void Render()
{
    if (g_RenderTarget == nullptr || g_Renderer == nullptr)
        return;

    const D2D1_SIZE_F size = g_RenderTarget->GetSize();

    g_RenderTarget->BeginDraw();
    g_RenderTarget->Clear(D2D1::ColorF(0.07f, 0.07f, 0.09f, 1.0f));

    // Status bar.
    g_Renderer->drawQuad(UIRect(0.0f, 0.0f, size.width, kStatusBarHeight), UIColor(0.14f, 0.14f, 0.17f, 1.0f));
    g_Renderer->drawText(UIRect(8.0f, 0.0f, size.width - 12.0f, kStatusBarHeight), BuildStatusText(), 13.0f,
                         UIColor(0.78f, 0.80f, 0.85f, 1.0f), TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

    // Timeline fills the rest.
    const float timeline_w = size.width;
    const float timeline_h = std::max(1.0f, size.height - kStatusBarHeight);
    if (g_FitOnNextPaint && !g_Snapshot.tracks.empty())
    {
        g_Timeline.FitView(timeline_w);
        g_FitOnNextPaint = false;
    }
    else
    {
        g_Timeline.EnsureInitialized(timeline_w);
    }

    ZSlate::FPaintContext ctx;
    ctx.Renderer = g_Renderer;
    ctx.LayerId = 0;
    const ZSlate::FGeometry geom(Vector2(0.0f, kStatusBarHeight), Vector2(timeline_w, timeline_h));
    g_Timeline.Paint(ctx, geom);

    const HRESULT hr = g_RenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        CreateRenderTarget();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_SIZE:
            if (g_RenderTarget)
            {
                g_RenderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        {
            SetCapture(hwnd);
            const Vector2 pos(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            g_Timeline.OnMouseButtonDown(pos, msg == WM_LBUTTONDOWN ? 0 : 2);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            const Vector2 pos(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            g_Timeline.OnMouseMove(pos);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        {
            const Vector2 pos(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            g_Timeline.OnMouseButtonUp(pos, msg == WM_LBUTTONUP ? 0 : 2);
            if (GetCapture() == hwnd)
                ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            POINT pt {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const Vector2 pos(static_cast<float>(pt.x), static_cast<float>(pt.y));
            const float delta = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.0f : -1.0f;
            g_Timeline.OnMouseWheel(pos, delta);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_CAPTURECHANGED:
            g_Timeline.OnMouseCaptureLost();
            return 0;

        case WM_KEYDOWN:
            if (wParam == 'O')
                OpenFileDialog();
            else if (wParam == 'F')
            {
                g_FitOnNextPaint = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_DROPFILES:
        {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            wchar_t path[MAX_PATH] = {};
            if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0)
                LoadTraceFile(path);
            DragFinish(drop);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_D2DFactory)))
    {
        MessageBoxW(nullptr, L"Failed to create the Direct2D factory.", L"ZEngine Insights", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_Renderer = new ZInsights::D2DUIRenderer();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    g_Hwnd = CreateWindowExW(0, kWindowClass, L"ZEngine Insights", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);
    if (g_Hwnd == nullptr)
    {
        MessageBoxW(nullptr, L"Failed to create the window.", L"ZEngine Insights", MB_OK | MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(g_Hwnd, TRUE);
    CreateRenderTarget();
    UpdateTitle();

    // Open the file passed on the command line (drag-onto-exe or editor launch).
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            if (argc >= 2 && argv[1][0] != L'\0')
                LoadTraceFile(argv[1]);
            LocalFree(argv);
        }
    }

    ShowWindow(g_Hwnd, nCmdShow);
    UpdateWindow(g_Hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_RenderTarget)
        g_RenderTarget->Release();
    delete g_Renderer;
    g_Renderer = nullptr;
    if (g_D2DFactory)
        g_D2DFactory->Release();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
