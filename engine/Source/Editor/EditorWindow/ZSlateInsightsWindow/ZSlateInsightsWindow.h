#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Profiler/InsightsTrace.h"
#include "Runtime/Profiler/SInsightsTimeline.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlateGeometry.h"

#include <memory>

namespace ZSlate
{
class SWidget;
class STextBlock;
class SButton;
}  // namespace ZSlate

// ZEngine Insights: an Unreal-Insights-style CPU timing profiler panel.
//
// Renders a per-thread flame chart (SInsightsTimeline) of the engine's
// Z_PROFILE_* scopes with a frame ruler on top. While the panel is visible it
// pulses InsightsTrace's capture heartbeat (unless paused), so capture overhead
// is paid only when someone is looking. Toolbar: Pause/Resume, Clear, Fit.
//
// Left-drag pans the timeline, mouse wheel zooms about the cursor; the status
// line shows the capture state, thread count, and the hovered scope's duration.
class ZSlateInsightsWindow : public EditorWindow
{
public:
    explicit ZSlateInsightsWindow(EditorUI* editor_ui);
    ~ZSlateInsightsWindow() override = default;

    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    // Write the current snapshot to a timestamped .ztrace under <cwd>/Insights/
    // and launch the standalone ZInsights.exe viewer on it. Returns the written
    // path (empty on failure). Static so the console command can reuse it.
    static std::string SaveTraceToDisk(const ZEngine::Insights::InsightsSnapshot& snapshot);
    static void LaunchStandaloneViewer(const std::string& trace_path);

private:
    void BuildLayout(float scale);
    void SaveAndOpenTrace();

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SInsightsTimeline> m_Timeline;
    std::shared_ptr<ZSlate::STextBlock> m_StatusText;
    std::shared_ptr<ZSlate::STextBlock> m_PauseLabel;

    ZEngine::Insights::InsightsSnapshot m_Snapshot;

    ZSlate::SlateInputRouter m_Input;
    float m_BuiltScale {-1.0f};
    bool m_Paused {false};
    bool m_PendingFit {false};
    bool m_PrevLeftDown {false};
};
