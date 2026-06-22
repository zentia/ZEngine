#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Function/Timeline/TimelineDirector.h"
#include "ZSlate/Application/SlateInput.h"
#include "Runtime/UI/Core/UITypes.h"

#include <memory>
#include <string>

namespace ZSlate
{
class SWidget;
class STextBlock;
}  // namespace ZSlate

class UIRenderer;
class TimelineAsset;
class TimelineTrack;
class TimelineClip;

// A ZSlate-rendered, dockable Timeline editor (dock title "Timeline").
//
// The toolbar (play/pause/stop, loop / snap toggles, zoom, edit mode, time
// read-out) is a persistent ZSlate widget tree built once per UI scale; the
// timeline canvas (ruler + track lanes + clips + playhead) is painted directly
// through the active UIRenderer (BatchedUIRenderer native backend, or the
// ImGui-draw-list fallback) and hit-tested manually -- it is a custom canvas,
// so it does not live in the widget tree. No ImGui widgets are used.
class ZSlateTimelineWindow : public EditorWindow
{
public:
    explicit ZSlateTimelineWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    // Public API preserved from the legacy ImGui TimelineWindow so external
    // callers (tests / future asset-open wiring) keep working.
    void SetTimelineAsset(TimelineAsset* asset);
    TimelineAsset* GetTimelineAsset() const { return m_CurrentAsset; }

    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    float GetTime() const { return m_CurrentTime; }
    TimelinePlayState GetPlayState() const { return m_PlayState; }

private:
    void BuildToolbar(float scale);
    void PaintCanvas(UIRenderer& renderer, const UIRect& region, float scale);
    void HandleCanvasMouse(const UIRect& region,
                           float scale,
                           const Vector2& mouse,
                           bool over_canvas,
                           bool clicked,
                           bool down,
                           bool released);
    void AdvancePlayback();

    // Time / pixel mapping (scaled pixels-per-second is computed per call site).
    int TimeToFrame(float time) const;
    float FrameToTime(int frame) const;

    // Visuals.
    UIColor TrackColor(TimelineTrack* track) const;
    UIColor ClipColor(TimelineClip* clip) const;
    const char* TrackTypeName(TimelineTrack* track) const;
    std::string ClipDisplayName(TimelineClip* clip) const;

    // Data ops.
    void AddTrack(const std::string& track_type);
    void DeleteTrack(int track_index);
    void SelectClip(TimelineTrack* track, int clip_index);
    bool IsClipSelected(TimelineTrack* track, int clip_index) const;
    void ClearSelection();
    void OnClipMoved(TimelineTrack* track, int clip_index, float new_start_time);
    bool CanMoveClip(TimelineTrack* track, int clip_index, float new_start_time) const;

    // ---- Model -------------------------------------------------------------
    TimelineAsset* m_CurrentAsset {nullptr};

    TimelinePlayState m_PlayState {TimelinePlayState::Stopped};
    float m_CurrentTime {0.0f};
    float m_PlaySpeed {1.0f};
    bool m_IsLooping {false};

    int m_EditMode {0};  // 0=Select 1=Move 2=Trim 3=Split
    bool m_SnapToFrames {true};
    bool m_SnapToClips {true};

    // Layout (unscaled px).
    float m_TrackListWidth {200.0f};
    float m_TimeRulerHeight {30.0f};
    float m_TrackHeight {40.0f};
    float m_PixelsPerSecond {100.0f};
    float m_TimeRangeStart {0.0f};

    // Selection.
    TimelineTrack* m_SelTrack {nullptr};
    int m_SelClipIndex {-1};

    // Clip drag.
    bool m_DraggingClip {false};
    float m_DragStartMouseX {0.0f};
    float m_DragStartTime {0.0f};
    TimelineTrack* m_DragTrack {nullptr};
    int m_DragClipIndex {-1};

    double m_LastFrameTime {0.0};

    // ---- ZSlate toolbar tree (rebuilt only on scale change) ----------------
    std::shared_ptr<ZSlate::SWidget> m_Toolbar;
    std::shared_ptr<ZSlate::STextBlock> m_PlayLabel;
    std::shared_ptr<ZSlate::STextBlock> m_TimeText;
    std::shared_ptr<ZSlate::STextBlock> m_SpeedText;
    std::shared_ptr<ZSlate::STextBlock> m_ZoomText;
    std::shared_ptr<ZSlate::STextBlock> m_ModeLabel;

    float m_BuiltScale {-1.0f};
    float m_ToolbarHeight {0.0f};

    ZSlate::SlateInputRouter m_Input;

    bool m_PrevLeftDown {false};
};
