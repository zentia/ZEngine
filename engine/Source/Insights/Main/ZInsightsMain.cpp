// ----------------------------------------------------------------------------
// ZInsights.exe -- the standalone ZEngine Insights viewer.
//
// Loads a `.ztrace` capture (written by the editor's Insights panel "Save &
// View" button, the `insights.dump` console command, or any future producer)
// and renders it through the shared SInsightsTimeline flame-chart widget --
// the exact same widget the in-editor Insights panel paints.
//
// The whole host is ZSlate::FSlateStandaloneApp, including the D3D11 render
// backend: window, DPI declaration, WM_DPICHANGED, font atlas, message pump
// and input router all come from the shell, so this file is only about what is
// Insights-specific -- loading the trace, the status bar and the shortcuts.
//
// Usage:  ZInsights.exe [path\to\capture.ztrace]
//         (or drag a .ztrace onto the window / press O to open / F to fit)
// ----------------------------------------------------------------------------

#include "CommonPCH/pch.h"  // windows.h, std, engine include dirs

#include <ZAppShell/AppMain.h>  // CommandLine (wide argv, owns its memory)

// Before <commdlg.h>: it includes <windows.h> and #undefs the winuser DrawText
// macro, keeping ZSlate's own DrawTextLabel the only text entry point.
#include <ZSlate/App/SlateStandaloneApp.h>
#include <ZSlate/Widgets/Layout/SBoxPanel.h>

#include <commdlg.h>

#include "Runtime/Profiler/InsightsTrace.h"
#include "Runtime/Profiler/SInsightsTimeline.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr float kStatusBarHeight = 26.0f;

ZEngine::Insights::InsightsSnapshot g_Snapshot;
std::shared_ptr<ZSlate::SInsightsTimeline> g_Timeline;
std::wstring g_TraceFile;
bool g_FitOnNextPaint = false;

ZSlate::FSlateStandaloneApp* g_App = nullptr;

// The status line: a background bar plus the hover / summary text. Auto-sized
// in the root vertical box, so it keeps its 26 logical pixels whatever the
// window height is.
class SInsightsStatusBar : public ZSlate::SLeafWidget
{
public:
    const ZEngine::Insights::InsightsSnapshot* Snapshot = nullptr;
    const ZSlate::SInsightsTimeline* Timeline = nullptr;

    ZSlate::Vector2 ComputeDesiredSize() const override { return ZSlate::Vector2(0.0f, kStatusBarHeight); }

    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const ZSlate::UIRect rect = geom.ToRect();
        ctx.Renderer->DrawQuad(rect, ZSlate::UIColor(0.14f, 0.14f, 0.17f, 1.0f));
        ctx.Renderer->DrawTextLabel(
            ZSlate::UIRect(rect.x + 8.0f, rect.y, rect.w - 12.0f, rect.h), BuildText(), 13.0f,
            ZSlate::UIColor(0.78f, 0.80f, 0.85f, 1.0f), ZSlate::TextAnchor::MiddleLeft,
            ZSlate::TextWrapMode::NoWrap);
    }

private:
    std::string BuildText() const
    {
        char buf[256];
        if (Snapshot == nullptr || Snapshot->tracks.empty())
        {
            std::snprintf(buf, sizeof(buf),
                          "No trace loaded.  [O] open  -  or drag a .ztrace onto the window");
            return buf;
        }
        std::snprintf(buf, sizeof(buf), "%zu threads  |  %zu frames    [O]pen  [F]it  scroll=zoom  drag=pan",
                      Snapshot->tracks.size(), Snapshot->frame_starts.size());
        std::string status = buf;
        if (Timeline != nullptr && !Timeline->HoverText().empty())
            status += "      " + Timeline->HoverText();
        return status;
    }
};

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

std::wstring ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

void UpdateTitle()
{
    if (g_App == nullptr)
        return;
    std::string title = "ZEngine Insights";
    if (!g_TraceFile.empty())
        title += "  -  " + ToUtf8(std::filesystem::path(g_TraceFile).filename().wstring());
    g_App->SetWindowTitle(title);
}

void LoadTraceFile(const std::wstring& path)
{
    ZEngine::Insights::InsightsSnapshot loaded;
    if (!ZEngine::Insights::LoadTrace(ToUtf8(path), loaded))
    {
        MessageBoxW(g_App ? g_App->GetHwnd() : nullptr, (L"Failed to load trace:\n" + path).c_str(),
                    L"ZEngine Insights", MB_OK | MB_ICONERROR);
        return;
    }
    g_Snapshot = std::move(loaded);
    if (g_Timeline)
        g_Timeline->SetSnapshot(&g_Snapshot);
    g_TraceFile = path;
    g_FitOnNextPaint = true;
    UpdateTitle();
}

void OpenFileDialog()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    if (g_App)
        ofn.hwndOwner = g_App->GetHwnd();
    ofn.lpstrFilter = L"ZEngine Insights Trace (*.ztrace)\0*.ztrace\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open .ztrace capture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        LoadTraceFile(file);
}
}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int nCmdShow)
{
    ZSlate::FStandaloneAppDesc desc;
    desc.Title = "ZEngine Insights";
    desc.Width = 1280.0f;
    desc.Height = 720.0f;
    desc.Font = ZSlate::Win32::ESystemFont::Sans;
    desc.AcceptFileDrops = true;
    desc.ClearColor = ZSlate::UIColor(0.07f, 0.07f, 0.09f, 1.0f);
    desc.ShowCommand = nCmdShow;
    // Window-class icons (title bar, Alt-Tab, taskbar). IDI_ICON1 comes from
    // z_embed_app_icon() in CMakeLists.txt and is only defined when that .rc
    // was actually embedded.
#ifdef IDI_ICON1
    desc.LargeIconResId = IDI_ICON1;
    desc.SmallIconResId = IDI_ICON1;
#endif

    ZSlate::FSlateStandaloneApp app;
    if (!app.Initialize(desc))
        return 1;
    g_App = &app;

    // ---- Content: status bar over the timeline, which fills the rest -------
    g_Timeline = std::make_shared<ZSlate::SInsightsTimeline>();
    auto status = std::make_shared<SInsightsStatusBar>();
    status->Snapshot = &g_Snapshot;
    status->Timeline = g_Timeline.get();

    auto root = std::make_shared<ZSlate::SVerticalBox>();
    root->AddSlot(status).AutoSize();
    root->AddSlot(g_Timeline).Fill(1.0f);
    app.SetRootContent(root);

    app.OnKey = [](unsigned vk, bool down)
    {
        if (!down)
            return false;
        if (vk == 'O')
        {
            OpenFileDialog();
            return true;
        }
        if (vk == 'F')
        {
            g_FitOnNextPaint = true;
            return true;
        }
        return false;
    };
    app.OnDropFile = [](const std::string& path)
    {
        LoadTraceFile(ToWide(path));
        return true;
    };

    // The timeline needs a width to fit into before its first paint; the panel
    // in the editor does the same from its own layout pass.
    app.OnFrame = [&app]
    {
        if (!g_Timeline)
            return;
        const float w = app.GetLogicalSize().x;
        if (g_FitOnNextPaint)
        {
            g_Timeline->FitView(w);
            g_FitOnNextPaint = false;
        }
        else
        {
            g_Timeline->EnsureInitialized(w);
        }
    };

    // Open the file passed on the command line (drag-onto-exe or editor launch).
    {
        const auto commandLine = zengine::appshell::CommandLine::Acquire();
        if (commandLine.valid() && commandLine.count() >= 2 && commandLine[1][0] != L'\0')
            LoadTraceFile(commandLine[1]);
    }

    const int result = app.Run();
    g_App = nullptr;
    return result;
}
