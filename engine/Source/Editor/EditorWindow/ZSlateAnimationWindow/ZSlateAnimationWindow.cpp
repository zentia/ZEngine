#include "ZSlateAnimationWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
using namespace ZSlate;

namespace
{
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kListBg(0.09f, 0.09f, 0.11f, 1.0f);
const ZSlate::UIColor kCanvasBg(0.118f, 0.118f, 0.118f, 1.0f);
const ZSlate::UIColor kRulerBg(0.157f, 0.157f, 0.157f, 1.0f);
const ZSlate::UIColor kGrid(0.235f, 0.235f, 0.235f, 1.0f);
const ZSlate::UIColor kSeparator(0.30f, 0.30f, 0.34f, 1.0f);
const ZSlate::UIColor kLabelColor(0.85f, 0.86f, 0.90f, 1.0f);
const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const ZSlate::UIColor kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ZSlate::UIColor kSelRowBg(0.20f, 0.30f, 0.45f, 1.0f);
const ZSlate::UIColor kPlayhead(1.0f, 0.10f, 0.10f, 1.0f);
const ZSlate::UIColor kKeyframe(1.0f, 0.78f, 0.0f, 1.0f);
const ZSlate::UIColor kKeyframeSel(1.0f, 1.0f, 0.0f, 1.0f);

constexpr float kRowH = 20.0f;  // unscaled property-list row height

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleLeft;
    return t;
}

// Draw a straight line as a chain of short axis-aligned quads (the UIRenderer
// has no native line primitive; works identically on both backends).
void DrawLineQuads(UIRenderer& r, const ZSlate::Vector2& a, const ZSlate::Vector2& b, const ZSlate::UIColor& color, float thickness)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float half = thickness * 0.5f;
    const int steps = std::max(1, static_cast<int>(dist / std::max(1.0f, half)));
    for (int i = 0; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float x = a.x + dx * t;
        const float y = a.y + dy * t;
        r.drawQuad(ZSlate::UIRect(x - half, y - half, thickness, thickness), color);
    }
}
}  // namespace

ZSlateAnimationWindow::ZSlateAnimationWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kAnimation)
{
    m_Open = false;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = ZSlate::FMargin(6.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Top;

    auto bar = std::make_shared<SHorizontalBox>();
    auto spacer = [&](float w) { bar->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(w * scale, 0.0f))).AutoSize(); };

    auto add_button = [&](std::shared_ptr<STextBlock> label, std::function<void()> on_click) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
        btn->SetContent(std::move(label));
        btn->OnClicked = std::move(on_click);
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center);
        spacer(5.0f);
    };
    auto add_check = [&](bool* flag, const char* text) {
        auto cb = std::make_shared<SCheckBox>();
        cb->Checked = *flag;
        cb->BoxSize = 15.0f * scale;
        cb->OnCheckStateChanged = [flag](bool b) { *flag = b; };
        bar->AddSlot(cb).AutoSize().SetVAlign(EVerticalAlignment::Center);
        spacer(2.0f);
        bar->AddSlot(MakeText(text, font, kLabelColor)).AutoSize().SetVAlign(EVerticalAlignment::Center);
        spacer(8.0f);
    };

    m_PlayLabel = MakeText(">", font, kLabelColor);
    m_PlayLabel->Alignment = ZSlate::TextAnchor::MiddleCenter;
    add_button(m_PlayLabel, [this]() {
        if (m_IsPlaying && !m_IsPaused)
            Pause();
        else
            Play();
    });
    {
        auto stop = MakeText("[]", font, kLabelColor);
        stop->Alignment = ZSlate::TextAnchor::MiddleCenter;
        add_button(stop, [this]() { Stop(); });
    }

    spacer(6.0f);
    m_TimeText = MakeText("Time: 0.000 (Frame: 0)", font, kDimColor);
    bar->AddSlot(m_TimeText).AutoSize().SetVAlign(EVerticalAlignment::Center);
    spacer(10.0f);

    add_button(MakeText("Speed -", font, kLabelColor), [this]() { m_PlaySpeed = std::max(0.1f, m_PlaySpeed - 0.1f); });
    m_SpeedText = MakeText("1.0x", font, kDimColor);
    bar->AddSlot(m_SpeedText).AutoSize().SetVAlign(EVerticalAlignment::Center);
    spacer(5.0f);
    add_button(MakeText("+", font, kLabelColor), [this]() { m_PlaySpeed = std::min(10.0f, m_PlaySpeed + 0.1f); });

    spacer(6.0f);
    add_check(&m_IsLooping, "Loop");
    add_check(&m_SnapToFrames, "Snap");

    add_button(MakeText("FPS -", font, kLabelColor), [this]() { m_FrameRate = std::max(1.0f, m_FrameRate - 1.0f); });
    m_FpsText = MakeText("30", font, kDimColor);
    bar->AddSlot(m_FpsText).AutoSize().SetVAlign(EVerticalAlignment::Center);
    spacer(5.0f);
    add_button(MakeText("+", font, kLabelColor), [this]() { m_FrameRate = std::min(120.0f, m_FrameRate + 1.0f); });

    spacer(6.0f);
    add_button(MakeText("Zoom -", font, kLabelColor),
               [this]() { m_PixelsPerSecond = std::max(10.0f, m_PixelsPerSecond * 0.8f); });
    m_ZoomText = MakeText("100 px/s", font, kDimColor);
    bar->AddSlot(m_ZoomText).AutoSize().SetVAlign(EVerticalAlignment::Center);
    spacer(5.0f);
    add_button(MakeText("+", font, kLabelColor),
               [this]() { m_PixelsPerSecond = std::min(1000.0f, m_PixelsPerSecond * 1.25f); });

    root->SetContent(bar);
    m_Toolbar = root;
}

// ---------------------------------------------------------------------------
// Property list (custom canvas)
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::PaintPropertyList(UIRenderer& r, const ZSlate::UIRect& region, float scale)
{
    r.drawQuad(region, kListBg);
    r.pushClipRect(region, true);
    m_PropRows.clear();

    const float row_h = kRowH * scale;
    const float font = 12.0f * scale;
    float y = region.y + 4.0f * scale;

    r.drawText(ZSlate::UIRect(region.x + 6.0f * scale, y, region.width - 8.0f * scale, row_h),
               "Properties",
               13.0f * scale,
               kLabelColor,
               ZSlate::TextAnchor::MiddleLeft,
               ZSlate::TextWrapMode::NoWrap);
    y += row_h;
    r.drawQuad(ZSlate::UIRect(region.x, y, region.width, 1.0f), kSeparator);
    y += 3.0f * scale;

    if (m_CurrentAsset == nullptr)
    {
        r.drawText(ZSlate::UIRect(region.x + 6.0f * scale, y, region.width - 8.0f * scale, row_h),
                   "(no asset)",
                   font,
                   kDimColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);
        r.popClipRect();
        return;
    }

    auto add_prop_row = [&](int channel_index, AnimPropType type, const char* prop_name) {
        const bool selected = (m_Selected.channel_index == channel_index && m_Selected.property_type == type);
        const ZSlate::UIRect rect(region.x, y, region.width, row_h);
        if (selected)
            r.drawQuad(rect, kSelRowBg);
        // type colour swatch
        r.drawQuad(ZSlate::UIRect(region.x + 22.0f * scale, y + row_h * 0.3f, 8.0f * scale, 8.0f * scale), PropertyColor(type));
        r.drawText(ZSlate::UIRect(region.x + 36.0f * scale, y, region.width - 40.0f * scale, row_h),
                   prop_name,
                   font,
                   selected ? kWhite : kLabelColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);
        m_PropRows.push_back(PropRow {rect, false, channel_index, std::string(), type});
        y += row_h;
    };

    for (size_t i = 0; i < m_CurrentAsset->clip_data.node_channels.size(); ++i)
    {
        const AnimationChannel& channel = m_CurrentAsset->clip_data.node_channels[i];
        const std::string key = channel.name + "_" + std::to_string(i);
        const bool expanded = m_ExpandedChannels.count(key) ? m_ExpandedChannels[key] : true;

        const ZSlate::UIRect head_rect(region.x, y, region.width, row_h);
        r.drawText(ZSlate::UIRect(region.x + 4.0f * scale, y, region.width - 8.0f * scale, row_h),
                   std::string(expanded ? "v " : "> ") + channel.name,
                   font,
                   kLabelColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);
        m_PropRows.push_back(PropRow {head_rect, true, static_cast<int>(i), key, AnimPropType::PositionX});
        y += row_h;

        if (!expanded)
            continue;

        if (!channel.position_keys.empty())
        {
            add_prop_row(static_cast<int>(i), AnimPropType::PositionX, "Position X");
            add_prop_row(static_cast<int>(i), AnimPropType::PositionY, "Position Y");
            add_prop_row(static_cast<int>(i), AnimPropType::PositionZ, "Position Z");
        }
        if (!channel.rotation_keys.empty())
        {
            add_prop_row(static_cast<int>(i), AnimPropType::RotationX, "Rotation X");
            add_prop_row(static_cast<int>(i), AnimPropType::RotationY, "Rotation Y");
            add_prop_row(static_cast<int>(i), AnimPropType::RotationZ, "Rotation Z");
        }
        if (!channel.scaling_keys.empty())
        {
            add_prop_row(static_cast<int>(i), AnimPropType::ScaleX, "Scale X");
            add_prop_row(static_cast<int>(i), AnimPropType::ScaleY, "Scale Y");
            add_prop_row(static_cast<int>(i), AnimPropType::ScaleZ, "Scale Z");
        }
    }

    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Curve editor (custom canvas)
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::PaintCurveEditor(UIRenderer& r, const ZSlate::UIRect& region, float scale)
{
    r.drawQuad(region, kCanvasBg);
    r.pushClipRect(region, true);

    if (m_CurrentAsset == nullptr || m_Selected.channel_index < 0)
    {
        r.drawText(region,
                   m_CurrentAsset == nullptr ? "No Animation Asset selected" : "Select a property to edit",
                   14.0f * scale,
                   kDimColor,
                   ZSlate::TextAnchor::MiddleCenter,
                   ZSlate::TextWrapMode::NoWrap);
        r.popClipRect();
        return;
    }

    const float pps = m_PixelsPerSecond * scale;

    // Auto-fit value range.
    if (m_AutoFitCurve && !m_Selected.keyframes.empty())
    {
        float lo, hi;
        ValueRange(m_Selected.keyframes, lo, hi);
        const float range = hi - lo;
        if (range > 0.001f)
        {
            m_CurveViewMin = lo - range * 0.1f;
            m_CurveViewMax = hi + range * 0.1f;
        }
    }
    const float vspan = std::max(0.001f, m_CurveViewMax - m_CurveViewMin);

    // Grid.
    const int rows = 10;
    for (int i = 0; i <= rows; ++i)
    {
        const float gy = region.y + (region.height / rows) * i;
        r.drawQuad(ZSlate::UIRect(region.x, gy, region.width, 1.0f), kGrid);
    }
    {
        const float visible_dur = region.width / pps;
        int cols = static_cast<int>(visible_dur * m_FrameRate / 10.0f);
        cols = std::min(50, std::max(1, cols));
        for (int i = 0; i <= cols; ++i)
        {
            const float time = m_TimeRangeStart + (visible_dur / cols) * i;
            const float px = (time - m_TimeRangeStart) * pps;
            if (px >= 0 && px <= region.width)
                r.drawQuad(ZSlate::UIRect(region.x + px, region.y, 1.0f, region.height), kGrid);
        }
    }

    auto value_to_y = [&](float v) { return region.y + region.height - (v - m_CurveViewMin) / vspan * region.height; };
    auto time_to_x = [&](float t) { return region.x + (t - m_TimeRangeStart) * pps; };

    // Curve (piecewise-linear over time-sorted keyframes).
    if (m_Selected.keyframes.size() >= 2)
    {
        std::vector<AnimKeyframe> sorted = m_Selected.keyframes;
        std::sort(sorted.begin(), sorted.end(), [](const AnimKeyframe& a, const AnimKeyframe& b) {
            return a.time < b.time;
        });
        const ZSlate::UIColor color = PropertyColor(m_Selected.property_type);
        for (size_t i = 0; i + 1 < sorted.size(); ++i)
        {
            const ZSlate::Vector2 p1(time_to_x(sorted[i].time), value_to_y(sorted[i].value));
            const ZSlate::Vector2 p2(time_to_x(sorted[i + 1].time), value_to_y(sorted[i + 1].value));
            DrawLineQuads(r, p1, p2, color, 2.0f * scale);
        }
    }

    // Keyframes.
    for (size_t i = 0; i < m_Selected.keyframes.size(); ++i)
    {
        const AnimKeyframe& kf = m_Selected.keyframes[i];
        const float x = time_to_x(kf.time);
        const float yv = value_to_y(kf.value);
        if (x < region.x || x > region.x + region.width)
            continue;
        const bool sel = (m_DraggingKeyframe == static_cast<int>(i));
        const float hr = 5.0f * scale;
        r.drawQuad(ZSlate::UIRect(x - hr, yv - hr, hr * 2.0f, hr * 2.0f), sel ? kKeyframeSel : kKeyframe);
        r.drawRect(ZSlate::UIRect(x - hr, yv - hr, hr * 2.0f, hr * 2.0f), kWhite, 1.0f);
    }

    // Playhead.
    const float head_x = (m_CurrentTime - m_TimeRangeStart) * pps;
    if (head_x >= 0 && head_x <= region.width)
        r.drawQuad(ZSlate::UIRect(region.x + head_x, region.y, 2.0f, region.height), kPlayhead);

    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Timeline (custom canvas)
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::PaintTimeline(UIRenderer& r, const ZSlate::UIRect& region, float scale)
{
    r.drawQuad(region, kRulerBg);
    if (m_CurrentAsset == nullptr)
        return;
    r.pushClipRect(region, true);

    const float pps = m_PixelsPerSecond * scale;
    const float visible_dur = region.width / pps;
    const int fps = std::max(1, static_cast<int>(m_FrameRate));
    const int start_frame = TimeToFrame(m_TimeRangeStart);
    const int end_frame = TimeToFrame(m_TimeRangeStart + visible_dur);

    for (int frame = start_frame; frame <= end_frame; frame += fps)
    {
        const float time = FrameToTime(frame);
        const float px = (time - m_TimeRangeStart) * pps;
        if (px < 0 || px > region.width)
            continue;
        const float x = region.x + px;
        r.drawQuad(ZSlate::UIRect(x, region.y, 1.0f, region.height), kWhite);
        char label[32];
        std::snprintf(label, sizeof(label), "%.2f", time);
        r.drawText(ZSlate::UIRect(x + 2.0f * scale, region.y + 1.0f * scale, 48.0f * scale, region.height * 0.5f),
                   label,
                   11.0f * scale,
                   kWhite,
                   ZSlate::TextAnchor::UpperLeft,
                   ZSlate::TextWrapMode::NoWrap);
    }
    if (pps > 50.0f)
    {
        for (int frame = start_frame; frame <= end_frame; ++frame)
        {
            const float px = (FrameToTime(frame) - m_TimeRangeStart) * pps;
            if (px < 0 || px > region.width)
                continue;
            r.drawQuad(ZSlate::UIRect(region.x + px, region.y + region.height * 0.7f, 1.0f, region.height * 0.3f),
                       ZSlate::UIColor(0.78f, 0.78f, 0.78f, 1.0f));
        }
    }

    // Keyframe markers for the selected property.
    if (m_Selected.channel_index >= 0)
    {
        for (const AnimKeyframe& kf : m_Selected.keyframes)
        {
            const float px = (kf.time - m_TimeRangeStart) * pps;
            if (px < 0 || px > region.width)
                continue;
            const float mh = 4.0f * scale;
            r.drawQuad(ZSlate::UIRect(region.x + px - mh, region.y + region.height * 0.5f - mh, mh * 2.0f, mh * 2.0f),
                       kKeyframeSel);
        }
    }

    const float head_x = (m_CurrentTime - m_TimeRangeStart) * pps;
    if (head_x >= 0 && head_x <= region.width)
        r.drawQuad(ZSlate::UIRect(region.x + head_x, region.y, 2.0f, region.height), kPlayhead);

    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::HandlePropertyListClick(const ZSlate::Vector2& mouse)
{
    for (const PropRow& row : m_PropRows)
    {
        if (!row.rect.Contains(mouse))
            continue;
        if (row.is_channel)
        {
            const bool cur = m_ExpandedChannels.count(row.channel_key) ? m_ExpandedChannels[row.channel_key] : true;
            m_ExpandedChannels[row.channel_key] = !cur;
        }
        else
        {
            m_Selected.channel_index = row.channel_index;
            if (m_CurrentAsset != nullptr && row.channel_index >= 0 &&
                row.channel_index < static_cast<int>(m_CurrentAsset->clip_data.node_channels.size()))
                m_Selected.channel_name = m_CurrentAsset->clip_data.node_channels[row.channel_index].name;
            m_Selected.property_type = row.prop_type;
            UpdateSelectedKeyframes();
        }
        return;
    }
}

void ZSlateAnimationWindow::HandleCurveInput(const ZSlate::UIRect& region,
                                             float scale,
                                             const ZSlate::Vector2& mouse,
                                             bool over,
                                             bool clicked,
                                             bool double_clicked,
                                             bool down,
                                             bool released)
{
    if (m_CurrentAsset == nullptr || m_Selected.channel_index < 0)
        return;

    const float pps = m_PixelsPerSecond * scale;
    const float vspan = std::max(0.001f, m_CurveViewMax - m_CurveViewMin);
    auto x_to_time = [&](float x) { return m_TimeRangeStart + (x - region.x) / pps; };

    // Continue keyframe drag (time only, value preserved -- matches legacy).
    if (m_DraggingKeyframe >= 0 && down)
    {
        if (m_DraggingKeyframe < static_cast<int>(m_Selected.keyframes.size()))
        {
            float new_time = x_to_time(mouse.x);
            if (m_SnapToFrames)
                new_time = FrameToTime(TimeToFrame(new_time));
            MoveKeyframe(m_DraggingKeyframe, new_time, m_DragStartValue);
        }
        return;
    }
    if (released)
        m_DraggingKeyframe = -1;

    if (!over)
        return;

    // Double-click adds a keyframe at the cursor.
    if (double_clicked)
    {
        float time = x_to_time(mouse.x);
        const float value = m_CurveViewMax - (mouse.y - region.y) / region.height * vspan;
        if (m_SnapToFrames)
            time = FrameToTime(TimeToFrame(time));
        AddKeyframe(time, value);
        return;
    }

    // Single click selects (and begins dragging) a nearby keyframe.
    if (clicked)
    {
        const float radius = 8.0f * scale;
        auto value_to_y = [&](float v) { return region.y + region.height - (v - m_CurveViewMin) / vspan * region.height; };
        for (size_t i = 0; i < m_Selected.keyframes.size(); ++i)
        {
            const float kx = region.x + (m_Selected.keyframes[i].time - m_TimeRangeStart) * pps;
            const float ky = value_to_y(m_Selected.keyframes[i].value);
            if ((mouse.x - kx) * (mouse.x - kx) + (mouse.y - ky) * (mouse.y - ky) <= radius * radius)
            {
                m_DraggingKeyframe = static_cast<int>(i);
                m_DragStartTime = m_Selected.keyframes[i].time;
                m_DragStartValue = m_Selected.keyframes[i].value;
                return;
            }
        }
    }
}

void ZSlateAnimationWindow::HandleTimelineClick(const ZSlate::UIRect& region, float scale, const ZSlate::Vector2& mouse)
{
    if (m_CurrentAsset == nullptr)
        return;
    const float pps = m_PixelsPerSecond * scale;
    float time = m_TimeRangeStart + (mouse.x - region.x) / pps;
    if (m_SnapToFrames)
        time = FrameToTime(TimeToFrame(time));
    SetTime(time);
}

// ---------------------------------------------------------------------------
// Per-frame entry point
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    if (m_Toolbar == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildToolbar(ui_scale);
        m_Input.Reset();
    }

    AdvancePlayback();

    if (m_PlayLabel)
        m_PlayLabel->Text = (m_IsPlaying && !m_IsPaused) ? "||" : ">";
    if (m_TimeText)
    {
        const int frame = TimeToFrame(m_CurrentTime);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Time: %.3f (Frame: %d)", FrameToTime(frame), frame);
        m_TimeText->Text = buf;
    }
    if (m_SpeedText)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fx", m_PlaySpeed);
        m_SpeedText->Text = buf;
    }
    if (m_FpsText)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f", m_FrameRate);
        m_FpsText->Text = buf;
    }
    if (m_ZoomText)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f px/s", m_PixelsPerSecond);
        m_ZoomText->Text = buf;
    }

    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    pos_x = native_rect[0];
    pos_y = native_rect[1];
    avail_w = native_rect[2];
    avail_h = native_rect[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    m_Toolbar->CacheDesiredSize();
    const float toolbar_h = std::min(avail_h, std::max(m_Toolbar->GetDesiredSize().y, 26.0f * ui_scale));
    m_ToolbarHeight = toolbar_h;

    const ZSlate::UIRect toolbar_region(pos_x, pos_y, avail_w, toolbar_h);
    const FGeometry toolbar_geom(ZSlate::Vector2(pos_x, pos_y), ZSlate::Vector2(avail_w, toolbar_h));

    const float content_y = pos_y + toolbar_h;
    const float content_h = avail_h - toolbar_h;
    const float list_w = std::min(avail_w * 0.5f, m_PropertyListWidth * ui_scale);
    const float right_x = pos_x + list_w;
    const float right_w = std::max(1.0f, avail_w - list_w);
    const float timeline_h = std::min(content_h * 0.5f, m_TimelineHeight * ui_scale);
    const float curve_h = std::max(1.0f, content_h - timeline_h);

    const ZSlate::UIRect list_region(pos_x, content_y, list_w, content_h);
    const ZSlate::UIRect curve_region(right_x, content_y, right_w, curve_h);
    const ZSlate::UIRect timeline_region(right_x, content_y + curve_h, right_w, timeline_h);
    const ZSlate::UIRect panel_region(pos_x, pos_y, avail_w, avail_h);

    // ---- Paint --------------------------------------------------------------
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();

    auto paint_all = [&](UIRenderer& r, FPaintContext& ctx) {
        PaintPropertyList(r, list_region, ui_scale);
        PaintCurveEditor(r, curve_region, ui_scale);
        PaintTimeline(r, timeline_region, ui_scale);
        // separators
        r.drawQuad(ZSlate::UIRect(right_x, content_y, 1.0f, content_h), kSeparator);
        r.drawQuad(ZSlate::UIRect(right_x, content_y + curve_h, right_w, 1.0f), kSeparator);

        r.pushClipRect(toolbar_region, true);
        m_Toolbar->Paint(ctx, toolbar_geom);
        r.popClipRect();
    };

    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        paint_all(renderer, ctx);
    }

    // ---- Input --------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, panel_region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_item = host.IsSurfaceHovered(surface_id, mouse);

    m_Input.ProcessMouse(m_Toolbar, mouse, over_item, host.IsLeftDown(), over_item ? host.GetWheelDelta() : 0.0f);

    const bool below_toolbar = over_item && mouse.y >= content_y;
    const bool left_clicked = below_toolbar && host.WasLeftPressedThisFrame();
    const bool left_down = host.IsLeftDown();
    const bool left_released = host.WasLeftReleasedThisFrame();
    const bool double_clicked = below_toolbar && host.WasLeftDoubleClickedThisFrame();

    if (left_clicked && list_region.Contains(mouse))
        HandlePropertyListClick(mouse);

    const bool over_curve = below_toolbar && curve_region.Contains(mouse);
    HandleCurveInput(curve_region,
                     ui_scale,
                     mouse,
                     over_curve,
                     left_clicked && curve_region.Contains(mouse),
                     double_clicked && curve_region.Contains(mouse),
                     left_down,
                     left_released);

    if (left_clicked && timeline_region.Contains(mouse))
        HandleTimelineClick(timeline_region, ui_scale, mouse);

    if (below_toolbar && mouse.x >= right_x && host.GetWheelDelta() != 0.0f)
    {
        m_TimeRangeStart -= host.GetWheelDelta() * (1.0f / m_PixelsPerSecond) * 60.0f;
        if (m_TimeRangeStart < 0.0f)
            m_TimeRangeStart = 0.0f;
    }

    if (over_item)
    {
        for (EKey key : host.GetKeysThisFrame())
        {
            if (key == EKey::Space)
            {
                if (m_IsPlaying && !m_IsPaused)
                    Pause();
                else
                    Play();
            }
            else if (key == EKey::Delete && m_DraggingKeyframe >= 0)
            {
                DeleteKeyframe(m_DraggingKeyframe);
                m_DraggingKeyframe = -1;
            }
        }
    }
}

void ZSlateAnimationWindow::AdvancePlayback()
{
    if (!m_IsPlaying || m_IsPaused || m_CurrentAsset == nullptr)
        return;

    const double now = ZSlate::EditorSlateHost::GetTime();
    float dt = (m_LastFrameTime > 0.0) ? static_cast<float>(now - m_LastFrameTime) : 0.0f;
    m_LastFrameTime = now;
    if (dt <= 0.0f || dt > 0.25f)
        dt = 0.016f;

    m_CurrentTime += dt * m_PlaySpeed;
    const float duration = (m_CurrentAsset->clip_data.total_frame > 0)
                               ? FrameToTime(m_CurrentAsset->clip_data.total_frame)
                               : 0.0f;
    if (duration > 0.0f && m_CurrentTime >= duration)
    {
        if (m_IsLooping)
            m_CurrentTime = 0.0f;
        else
            Stop();
    }
}

// ---------------------------------------------------------------------------
// Public API + ops
// ---------------------------------------------------------------------------
void ZSlateAnimationWindow::SetAnimationAsset(AnimationAsset* asset)
{
    m_CurrentAsset = asset;
    m_Selected = AnimSelectedProperty {};
    if (asset != nullptr && asset->clip_data.total_frame > 0)
        Stop();
}

void ZSlateAnimationWindow::Play()
{
    m_IsPlaying = true;
    m_IsPaused = false;
    m_LastFrameTime = ZSlate::EditorSlateHost::GetTime();
}

void ZSlateAnimationWindow::Pause()
{
    m_IsPaused = true;
}

void ZSlateAnimationWindow::Stop()
{
    m_IsPlaying = false;
    m_IsPaused = false;
    m_CurrentTime = 0.0f;
}

void ZSlateAnimationWindow::SetTime(float time)
{
    m_CurrentTime = std::max(0.0f, time);
}

void ZSlateAnimationWindow::AddKeyframe(float time, float value)
{
    m_Selected.keyframes.push_back(AnimKeyframe {time, value});
    std::sort(m_Selected.keyframes.begin(), m_Selected.keyframes.end(), [](const AnimKeyframe& a, const AnimKeyframe& b) {
        return a.time < b.time;
    });
}

void ZSlateAnimationWindow::DeleteKeyframe(int index)
{
    if (index >= 0 && index < static_cast<int>(m_Selected.keyframes.size()))
        m_Selected.keyframes.erase(m_Selected.keyframes.begin() + index);
}

void ZSlateAnimationWindow::MoveKeyframe(int index, float new_time, float new_value)
{
    if (index < 0 || index >= static_cast<int>(m_Selected.keyframes.size()))
        return;
    m_Selected.keyframes[index].time = new_time;
    m_Selected.keyframes[index].value = new_value;
}

void ZSlateAnimationWindow::UpdateSelectedKeyframes()
{
    if (m_CurrentAsset == nullptr || m_Selected.channel_index < 0 ||
        m_Selected.channel_index >= static_cast<int>(m_CurrentAsset->clip_data.node_channels.size()))
    {
        m_Selected.keyframes.clear();
        return;
    }
    m_Selected.keyframes =
        ExtractKeyframes(m_CurrentAsset->clip_data.node_channels[m_Selected.channel_index], m_Selected.property_type);
}

std::vector<AnimKeyframe> ZSlateAnimationWindow::ExtractKeyframes(const AnimationChannel& channel,
                                                                  AnimPropType type) const
{
    std::vector<AnimKeyframe> out;
    switch (type)
    {
        case AnimPropType::PositionX:
        case AnimPropType::PositionY:
        case AnimPropType::PositionZ:
        {
            const int c = static_cast<int>(type) - static_cast<int>(AnimPropType::PositionX);
            for (size_t i = 0; i < channel.position_keys.size(); ++i)
                out.push_back(AnimKeyframe {FrameToTime(static_cast<int>(i)), channel.position_keys[i][c]});
            break;
        }
        case AnimPropType::RotationX:
        case AnimPropType::RotationY:
        case AnimPropType::RotationZ:
        {
            // Quaternion -> Euler conversion is out of scope (matches legacy placeholder).
            for (size_t i = 0; i < channel.rotation_keys.size(); ++i)
                out.push_back(AnimKeyframe {FrameToTime(static_cast<int>(i)), 0.0f});
            break;
        }
        case AnimPropType::ScaleX:
        case AnimPropType::ScaleY:
        case AnimPropType::ScaleZ:
        {
            const int c = static_cast<int>(type) - static_cast<int>(AnimPropType::ScaleX);
            for (size_t i = 0; i < channel.scaling_keys.size(); ++i)
                out.push_back(AnimKeyframe {FrameToTime(static_cast<int>(i)), channel.scaling_keys[i][c]});
            break;
        }
    }
    return out;
}

int ZSlateAnimationWindow::TimeToFrame(float time) const
{
    return static_cast<int>(time * m_FrameRate);
}

float ZSlateAnimationWindow::FrameToTime(int frame) const
{
    return m_FrameRate > 0.0f ? static_cast<float>(frame) / m_FrameRate : 0.0f;
}

ZSlate::UIColor ZSlateAnimationWindow::PropertyColor(AnimPropType type) const
{
    switch (type)
    {
        case AnimPropType::PositionX:
            return ZSlate::UIColor(1.0f, 0.39f, 0.39f, 1.0f);
        case AnimPropType::PositionY:
            return ZSlate::UIColor(0.39f, 1.0f, 0.39f, 1.0f);
        case AnimPropType::PositionZ:
            return ZSlate::UIColor(0.39f, 0.39f, 1.0f, 1.0f);
        case AnimPropType::RotationX:
            return ZSlate::UIColor(1.0f, 0.78f, 0.39f, 1.0f);
        case AnimPropType::RotationY:
            return ZSlate::UIColor(0.78f, 1.0f, 0.39f, 1.0f);
        case AnimPropType::RotationZ:
            return ZSlate::UIColor(0.39f, 0.78f, 1.0f, 1.0f);
        case AnimPropType::ScaleX:
            return ZSlate::UIColor(1.0f, 0.39f, 0.78f, 1.0f);
        case AnimPropType::ScaleY:
            return ZSlate::UIColor(0.78f, 0.39f, 1.0f, 1.0f);
        case AnimPropType::ScaleZ:
            return ZSlate::UIColor(0.39f, 1.0f, 0.78f, 1.0f);
        default:
            return ZSlate::UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

void ZSlateAnimationWindow::ValueRange(const std::vector<AnimKeyframe>& kfs, float& min_val, float& max_val) const
{
    if (kfs.empty())
    {
        min_val = -1.0f;
        max_val = 1.0f;
        return;
    }
    min_val = kfs[0].value;
    max_val = kfs[0].value;
    for (const AnimKeyframe& kf : kfs)
    {
        min_val = std::min(min_val, kf.value);
        max_val = std::max(max_val, kf.value);
    }
}
