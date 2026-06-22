#include "ZSlateInsightsWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Core/Base/Macro.h"
#include "ZSlate/Widgets/SBorder.h"
#include "ZSlate/Widgets/SBoxPanel.h"
#include "ZSlate/Widgets/SButton.h"
#include "ZSlate/Widgets/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#ifdef _WIN32
    #include <shellapi.h>
    #include <windows.h>
#endif

using namespace ZSlate;

namespace
{
const UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const UIColor kLabelColor(0.82f, 0.84f, 0.88f, 1.0f);
const UIColor kDimColor(0.58f, 0.60f, 0.66f, 1.0f);

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = TextAnchor::MiddleLeft;
    return t;
}
}  // namespace

ZSlateInsightsWindow::ZSlateInsightsWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kInsights)
{
    // Retain a bit more than two seconds at 60 FPS so a stall is easy to find.
    ZEngine::Insights::InsightsTrace::Get().SetRetainedFrames(150);
}

void ZSlateInsightsWindow::BuildLayout(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = FMargin(4.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();

    // ---- Toolbar -----------------------------------------------------------
    auto bar = std::make_shared<SHorizontalBox>();

    auto pause_btn = std::make_shared<SButton>();
    pause_btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    m_PauseLabel = MakeText("Pause", font, kLabelColor);
    pause_btn->SetContent(m_PauseLabel);
    pause_btn->OnClicked = [this]() { m_Paused = !m_Paused; };
    bar->AddSlot(pause_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();

    auto clear_btn = std::make_shared<SButton>();
    clear_btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    clear_btn->SetContent(MakeText("Clear", font, kLabelColor));
    clear_btn->OnClicked = [this]() { ZEngine::Insights::InsightsTrace::Get().Clear(); };
    bar->AddSlot(clear_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();

    auto fit_btn = std::make_shared<SButton>();
    fit_btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    fit_btn->SetContent(MakeText("Fit", font, kLabelColor));
    fit_btn->OnClicked = [this]() { m_PendingFit = true; };
    bar->AddSlot(fit_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();

    auto save_btn = std::make_shared<SButton>();
    save_btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    save_btn->SetContent(MakeText("Save & View", font, kLabelColor));
    save_btn->OnClicked = [this]() { SaveAndOpenTrace(); };
    bar->AddSlot(save_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(10.0f * scale, 0.0f))).AutoSize();

    m_StatusText = MakeText("", font, kDimColor);
    bar->AddSlot(m_StatusText).Fill().SetVAlign(EVerticalAlignment::Center);

    column->AddSlot(bar).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Timeline ----------------------------------------------------------
    auto timeline_border = std::make_shared<SBorder>();
    timeline_border->BackgroundColor = UIColor(0.07f, 0.07f, 0.09f, 1.0f);
    timeline_border->Padding = FMargin(0.0f, 0.0f);
    timeline_border->HAlign = EHorizontalAlignment::Fill;
    timeline_border->VAlign = EVerticalAlignment::Fill;
    m_Timeline = std::make_shared<SInsightsTimeline>();
    timeline_border->SetContent(m_Timeline);
    column->AddSlot(timeline_border).Fill();

    root->SetContent(column);
    m_Root = root;
}

void ZSlateInsightsWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    if (m_Root == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildLayout(ui_scale);
        m_Input.Reset();
    }

    // Pulse the capture heartbeat while visible and not paused. Capture auto-stops
    // a couple frames after we stop pulsing (panel closed / paused) -- see
    // InsightsTrace::EndFrame.
    ZEngine::Insights::InsightsTrace& trace = ZEngine::Insights::InsightsTrace::Get();
    if (!m_Paused)
        trace.RequestCapture();

    trace.BuildSnapshot(m_Snapshot);
    if (m_Timeline)
        m_Timeline->SetSnapshot(&m_Snapshot);

    // ---- Geometry ----------------------------------------------------------
    const float* native_rect = NativeRect();
    Vector2 pos(native_rect[0], native_rect[1]);
    Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    if (m_Timeline)
    {
        m_Timeline->EnsureInitialized(avail.x);
        if (m_PendingFit)
        {
            m_Timeline->FitView(avail.x);
            m_PendingFit = false;
        }
    }

    // ---- Status / labels ---------------------------------------------------
    if (m_PauseLabel)
        m_PauseLabel->Text = m_Paused ? "Resume" : "Pause";
    if (m_StatusText)
    {
        const char* state = m_Paused ? "paused" : (trace.IsCapturing() ? "capturing" : "idle");
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s  |  %zu threads  |  %zu frames", state, m_Snapshot.tracks.size(),
                      m_Snapshot.frame_starts.size());
        std::string status = buf;
        if (m_Timeline && !m_Timeline->HoverText().empty())
            status += "      " + m_Timeline->HoverText();
        m_StatusText->Text = status;
    }

    // ---- Paint -------------------------------------------------------------
    const UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(Vector2(pos.x, pos.y), Vector2(avail.x, avail.y));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        renderer.pushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.popClipRect();
    }

    // ---- Input -------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);
    m_PrevLeftDown = left_down;
}

std::string ZSlateInsightsWindow::SaveTraceToDisk(const ZEngine::Insights::InsightsSnapshot& snapshot)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path dir = fs::current_path(ec) / "Insights";
    fs::create_directories(dir, ec);

    const std::time_t now = std::time(nullptr);
    std::tm tm_buf {};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);

    const fs::path out_path = dir / (std::string("trace_") + stamp + ".ztrace");
    const std::string out_str = out_path.generic_string();

    if (!ZEngine::Insights::SaveTrace(out_str, snapshot))
    {
        LOG_ERROR(ZEditor, "Insights: failed to write trace to {}", out_str);
        return std::string();
    }
    LOG_INFO(ZEditor, "Insights: saved trace ({} tracks, {} frames) to {}", snapshot.tracks.size(),
             snapshot.frame_starts.size(), out_str);
    return out_str;
}

void ZSlateInsightsWindow::LaunchStandaloneViewer(const std::string& trace_path)
{
    if (trace_path.empty())
        return;
#ifdef _WIN32
    // ZInsights.exe sits next to ZEditor.exe in the same bin/ directory.
    wchar_t module_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0)
    {
        LOG_WARNING(ZEditor, "Insights: could not resolve module path to launch ZInsights");
        return;
    }
    std::filesystem::path viewer = std::filesystem::path(module_path).parent_path() / "ZInsights.exe";
    if (!std::filesystem::exists(viewer))
    {
        LOG_WARNING(ZEditor, "Insights: ZInsights.exe not found next to the editor ({}); trace saved only",
                    viewer.generic_string());
        return;
    }
    const std::wstring viewer_w = viewer.wstring();
    const std::wstring arg_w = L"\"" + std::filesystem::path(trace_path).wstring() + L"\"";
    const HINSTANCE rc =
        ShellExecuteW(nullptr, L"open", viewer_w.c_str(), arg_w.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
    {
        LOG_WARNING(ZEditor, "Insights: ShellExecute failed to launch ZInsights.exe (code {})",
                    reinterpret_cast<INT_PTR>(rc));
    }
#else
    LOG_INFO(ZEditor, "Insights: trace saved to {} (standalone viewer is Windows-only)", trace_path);
#endif
}

void ZSlateInsightsWindow::SaveAndOpenTrace()
{
    const std::string path = SaveTraceToDisk(m_Snapshot);
    LaunchStandaloneViewer(path);
}
