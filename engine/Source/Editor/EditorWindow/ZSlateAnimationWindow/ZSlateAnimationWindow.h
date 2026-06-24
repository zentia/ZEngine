#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "ZSlate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "ZSlate/Application/SlateInput.h"
#include "Runtime/UI/Core/UITypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZSlate
{
class SWidget;
class STextBlock;
}  // namespace ZSlate

class UIRenderer;

// Animated property selector (one scalar curve at a time).
enum class AnimPropType
{
    PositionX,
    PositionY,
    PositionZ,
    RotationX,
    RotationY,
    RotationZ,
    ScaleX,
    ScaleY,
    ScaleZ
};

struct AnimKeyframe
{
    float time {0.0f};
    float value {0.0f};
};

struct AnimSelectedProperty
{
    int channel_index {-1};
    std::string channel_name;
    AnimPropType property_type {AnimPropType::PositionX};
    std::vector<AnimKeyframe> keyframes;
};

// A ZSlate-rendered, dockable Animation curve editor (dock title "Animation").
//
// Toolbar (play/pause/stop, speed, loop / snap toggles, fps, zoom, time
// read-out) is a persistent ZSlate widget tree. The three working areas --
// property list (left), curve editor (top-right) and timeline (bottom-right) --
// are custom canvases painted directly through the active UIRenderer
// (DrawQuad / DrawRect / DrawText; curves are drawn as short sampled quads since
// the renderer has no native line primitive) and hit-tested manually. No ImGui
// widgets are used.
class ZSlateAnimationWindow : public EditorWindow
{
public:
    explicit ZSlateAnimationWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    void SetAnimationAsset(AnimationAsset* asset);
    AnimationAsset* GetAnimationAsset() const { return m_CurrentAsset; }

    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    float GetTime() const { return m_CurrentTime; }
    bool IsPlaying() const { return m_IsPlaying; }

private:
    void BuildToolbar(float scale);
    void AdvancePlayback();

    void PaintPropertyList(UIRenderer& r, const UIRect& region, float scale);
    void PaintCurveEditor(UIRenderer& r, const UIRect& region, float scale);
    void PaintTimeline(UIRenderer& r, const UIRect& region, float scale);

    void HandlePropertyListClick(const Vector2& mouse);
    void HandleCurveInput(const UIRect& region,
                          float scale,
                          const Vector2& mouse,
                          bool over,
                          bool clicked,
                          bool double_clicked,
                          bool down,
                          bool released);
    void HandleTimelineClick(const UIRect& region, float scale, const Vector2& mouse);

    // Keyframe ops.
    void AddKeyframe(float time, float value);
    void DeleteKeyframe(int index);
    void MoveKeyframe(int index, float new_time, float new_value);
    void UpdateSelectedKeyframes();
    std::vector<AnimKeyframe> ExtractKeyframes(const AnimationChannel& channel, AnimPropType type) const;

    // Time / value mapping.
    int TimeToFrame(float time) const;
    float FrameToTime(int frame) const;

    ZSlate::UIColor PropertyColor(AnimPropType type) const;
    void ValueRange(const std::vector<AnimKeyframe>& kfs, float& min_val, float& max_val) const;

    // ---- Model -------------------------------------------------------------
    AnimationAsset* m_CurrentAsset {nullptr};

    bool m_IsPlaying {false};
    bool m_IsPaused {false};
    float m_CurrentTime {0.0f};
    float m_PlaySpeed {1.0f};
    bool m_IsLooping {false};
    float m_FrameRate {30.0f};

    AnimSelectedProperty m_Selected;

    float m_PropertyListWidth {250.0f};
    float m_TimelineHeight {110.0f};
    float m_PixelsPerSecond {100.0f};
    float m_TimeRangeStart {0.0f};

    float m_CurveViewMin {-10.0f};
    float m_CurveViewMax {10.0f};
    bool m_AutoFitCurve {true};

    bool m_SnapToFrames {true};

    int m_DraggingKeyframe {-1};
    float m_DragStartTime {0.0f};
    float m_DragStartValue {0.0f};

    double m_LastFrameTime {0.0};

    std::unordered_map<std::string, bool> m_ExpandedChannels;

    // Property-list row hit-testing (rebuilt each paint).
    struct PropRow
    {
        UIRect rect;
        bool is_channel {false};
        int channel_index {-1};
        std::string channel_key;
        AnimPropType prop_type {AnimPropType::PositionX};
    };
    std::vector<PropRow> m_PropRows;

    // ---- ZSlate toolbar ----------------------------------------------------
    std::shared_ptr<ZSlate::SWidget> m_Toolbar;
    std::shared_ptr<ZSlate::STextBlock> m_PlayLabel;
    std::shared_ptr<ZSlate::STextBlock> m_TimeText;
    std::shared_ptr<ZSlate::STextBlock> m_SpeedText;
    std::shared_ptr<ZSlate::STextBlock> m_FpsText;
    std::shared_ptr<ZSlate::STextBlock> m_ZoomText;

    float m_BuiltScale {-1.0f};
    float m_ToolbarHeight {0.0f};

    ZSlate::SlateInputRouter m_Input;
};
