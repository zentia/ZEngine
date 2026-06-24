#include "ZSlateTimelineWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"      // native input / metrics
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"
#include "Runtime/Resource/ResType/Data/TimelineClip.h"
#include "Runtime/Resource/ResType/Data/TimelineTrack.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/SCheckBox.h"
#include "ZSlate/Widgets/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <cstdio>
#include <functional>
using namespace ZSlate;

namespace
{
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kCanvasBg(0.13f, 0.13f, 0.15f, 1.0f);
const ZSlate::UIColor kRulerBg(0.157f, 0.157f, 0.157f, 1.0f);
const ZSlate::UIColor kTrackListBg(0.09f, 0.09f, 0.11f, 1.0f);
const ZSlate::UIColor kSeparator(0.30f, 0.30f, 0.34f, 1.0f);
const ZSlate::UIColor kLabelColor(0.82f, 0.84f, 0.88f, 1.0f);
const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const ZSlate::UIColor kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ZSlate::UIColor kBlack(0.0f, 0.0f, 0.0f, 1.0f);
const ZSlate::UIColor kPlayhead(1.0f, 0.10f, 0.10f, 1.0f);
const ZSlate::UIColor kClipSelected(1.0f, 1.0f, 0.0f, 1.0f);

const char* kModeNames[] = {"Select", "Move", "Trim", "Split"};

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleLeft;
    return t;
}
}  // namespace

ZSlateTimelineWindow::ZSlateTimelineWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kTimeline)
{
    m_Open = false;
}

// ---------------------------------------------------------------------------
// Toolbar (ZSlate widget tree)
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::BuildToolbar(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = ZSlate::FMargin(6.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Top;

    auto bar = std::make_shared<SHorizontalBox>();

    auto add_spacer = [&](float w) {
        bar->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(w * scale, 0.0f))).AutoSize();
    };

    auto add_button = [&](std::shared_ptr<STextBlock> label, std::function<void()> on_click) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
        btn->SetContent(std::move(label));
        btn->OnClicked = std::move(on_click);
        bar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center);
        add_spacer(5.0f);
    };

    auto add_check = [&](bool* flag, const char* text) {
        auto cb = std::make_shared<SCheckBox>();
        cb->Checked = *flag;
        cb->BoxSize = 15.0f * scale;
        cb->OnCheckStateChanged = [flag](bool b) { *flag = b; };
        bar->AddSlot(cb).AutoSize().SetVAlign(EVerticalAlignment::Center);
        add_spacer(2.0f);
        bar->AddSlot(MakeText(text, font, kLabelColor)).AutoSize().SetVAlign(EVerticalAlignment::Center);
        add_spacer(8.0f);
    };

    // Play / Pause (label toggles in OnGUI).
    m_PlayLabel = MakeText(">", font, kLabelColor);
    m_PlayLabel->Alignment = ZSlate::TextAnchor::MiddleCenter;
    add_button(m_PlayLabel, [this]() {
        if (m_PlayState == TimelinePlayState::Playing)
            Pause();
        else
            Play();
    });

    // Stop.
    {
        auto stop_label = MakeText("[]", font, kLabelColor);
        stop_label->Alignment = ZSlate::TextAnchor::MiddleCenter;
        add_button(stop_label, [this]() { Stop(); });
    }

    add_spacer(6.0f);

    // Time read-out.
    m_TimeText = MakeText("Time: 0.000 (Frame: 0)", font, kDimColor);
    bar->AddSlot(m_TimeText).AutoSize().SetVAlign(EVerticalAlignment::Center);
    add_spacer(10.0f);

    // Speed - / value / +.
    {
        auto minus = MakeText("Speed -", font, kLabelColor);
        add_button(minus, [this]() { m_PlaySpeed = std::max(0.1f, m_PlaySpeed - 0.1f); });
        m_SpeedText = MakeText("1.0x", font, kDimColor);
        bar->AddSlot(m_SpeedText).AutoSize().SetVAlign(EVerticalAlignment::Center);
        add_spacer(5.0f);
        auto plus = MakeText("+", font, kLabelColor);
        add_button(plus, [this]() { m_PlaySpeed = std::min(10.0f, m_PlaySpeed + 0.1f); });
    }

    add_spacer(6.0f);

    add_check(&m_IsLooping, "Loop");
    add_check(&m_SnapToFrames, "Snap Frames");
    add_check(&m_SnapToClips, "Snap Clips");

    // Zoom - / value / +.
    {
        auto minus = MakeText("Zoom -", font, kLabelColor);
        add_button(minus, [this]() { m_PixelsPerSecond = std::max(10.0f, m_PixelsPerSecond * 0.8f); });
        m_ZoomText = MakeText("100 px/s", font, kDimColor);
        bar->AddSlot(m_ZoomText).AutoSize().SetVAlign(EVerticalAlignment::Center);
        add_spacer(5.0f);
        auto plus = MakeText("+", font, kLabelColor);
        add_button(plus, [this]() { m_PixelsPerSecond = std::min(1000.0f, m_PixelsPerSecond * 1.25f); });
    }

    add_spacer(6.0f);

    // Edit mode cycle.
    m_ModeLabel = MakeText("Mode: Select", font, kLabelColor);
    add_button(m_ModeLabel, [this]() { m_EditMode = (m_EditMode + 1) % 4; });

    // Add track.
    {
        auto add_label = MakeText("+ Animation Track", font, kLabelColor);
        add_button(add_label, [this]() { AddTrack("AnimationTimelineTrack"); });
    }

    root->SetContent(bar);
    m_Toolbar = root;
}

// ---------------------------------------------------------------------------
// Canvas paint (ruler + track lanes + clips + playhead)
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::PaintCanvas(UIRenderer& r, const ZSlate::UIRect& region, float scale)
{
    r.drawQuad(region, kCanvasBg);

    if (m_CurrentAsset == nullptr)
    {
        r.drawText(region,
                   "No Timeline Asset selected",
                   14.0f * scale,
                   kDimColor,
                   ZSlate::TextAnchor::MiddleCenter,
                   ZSlate::TextWrapMode::NoWrap);
        return;
    }

    const float track_list_w = m_TrackListWidth * scale;
    const float ruler_h = m_TimeRulerHeight * scale;
    const float track_h = m_TrackHeight * scale;
    const float pps = m_PixelsPerSecond * scale;

    const float right_x = region.x + track_list_w;
    const float right_w = std::max(1.0f, region.width - track_list_w);

    // Track-list background + separator.
    r.drawQuad(ZSlate::UIRect(region.x, region.y, track_list_w, region.height), kTrackListBg);
    r.drawQuad(ZSlate::UIRect(right_x, region.y, 1.0f, region.height), kSeparator);

    r.drawText(ZSlate::UIRect(region.x + 6.0f * scale, region.y + 2.0f * scale, track_list_w - 8.0f * scale, ruler_h),
               "Tracks",
               13.0f * scale,
               kLabelColor,
               ZSlate::TextAnchor::MiddleLeft,
               ZSlate::TextWrapMode::NoWrap);

    // ---- Ruler --------------------------------------------------------------
    const ZSlate::UIRect ruler(right_x, region.y, right_w, ruler_h);
    r.drawQuad(ruler, kRulerBg);
    r.pushClipRect(ruler, true);

    const float visible_duration = right_w / pps;
    const int start_frame = TimeToFrame(m_TimeRangeStart);
    const int end_frame = TimeToFrame(m_TimeRangeStart + visible_duration);
    const int fps = std::max(1, static_cast<int>(m_CurrentAsset->m_FrameRate));

    for (int frame = start_frame; frame <= end_frame; frame += fps)
    {
        const float time = FrameToTime(frame);
        const float px = (time - m_TimeRangeStart) * pps;
        if (px < 0.0f || px > right_w)
            continue;
        const float x = right_x + px;
        r.drawQuad(ZSlate::UIRect(x, region.y, 1.0f, ruler_h), kWhite);

        char label[32];
        std::snprintf(label, sizeof(label), "%.2f", time);
        r.drawText(ZSlate::UIRect(x + 2.0f * scale, region.y + 1.0f * scale, 48.0f * scale, ruler_h * 0.6f),
                   label,
                   11.0f * scale,
                   kWhite,
                   ZSlate::TextAnchor::UpperLeft,
                   ZSlate::TextWrapMode::NoWrap);
    }

    // Sub-ticks per frame when zoomed in.
    if (pps > 50.0f)
    {
        for (int frame = start_frame; frame <= end_frame; ++frame)
        {
            const float px = (FrameToTime(frame) - m_TimeRangeStart) * pps;
            if (px < 0.0f || px > right_w)
                continue;
            const float x = right_x + px;
            r.drawQuad(ZSlate::UIRect(x, region.y + ruler_h * 0.7f, 1.0f, ruler_h * 0.3f), ZSlate::UIColor(0.78f, 0.78f, 0.78f, 1.0f));
        }
    }
    r.popClipRect();

    // ---- Track lanes + clips ------------------------------------------------
    const float lanes_top = region.y + ruler_h;
    const ZSlate::UIRect lanes_clip(right_x, lanes_top, right_w, region.height - ruler_h);
    r.pushClipRect(lanes_clip, true);

    float y = lanes_top;
    for (size_t i = 0; i < m_CurrentAsset->m_Tracks.size(); ++i)
    {
        TimelineTrack* track = m_CurrentAsset->m_Tracks[i];
        if (track == nullptr)
            continue;

        // Lane background.
        const ZSlate::UIRect lane(right_x, y, right_w, track_h);
        r.drawQuad(lane, TrackColor(track));

        // Track header in left column.
        r.drawText(ZSlate::UIRect(region.x + 8.0f * scale, y, track_list_w - 12.0f * scale, track_h),
                   track->m_Name.empty() ? std::string(TrackTypeName(track)) : track->m_Name,
                   12.0f * scale,
                   kLabelColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);

        // Clips.
        for (size_t j = 0; j < track->m_Clips.size(); ++j)
        {
            TimelineClip* clip = track->m_Clips[j];
            if (clip == nullptr || !clip->m_Enabled)
                continue;

            const float clip_x = right_x + (clip->m_StartTime - m_TimeRangeStart) * pps;
            const float clip_w = clip->m_Duration * pps;
            if (clip_x + clip_w < right_x || clip_x > right_x + right_w)
                continue;

            const ZSlate::UIRect clip_rect(clip_x, y + 2.0f * scale, clip_w, track_h - 4.0f * scale);
            const bool selected = IsClipSelected(track, static_cast<int>(j));
            r.drawQuad(clip_rect, selected ? kClipSelected : ClipColor(clip));
            r.drawRect(clip_rect, kWhite, 1.0f);

            if (clip_w > 50.0f * scale)
            {
                r.drawText(ZSlate::UIRect(clip_rect.x + 4.0f * scale, clip_rect.y, clip_rect.width - 6.0f * scale, clip_rect.height),
                           ClipDisplayName(clip),
                           11.0f * scale,
                           kBlack,
                           ZSlate::TextAnchor::MiddleLeft,
                           ZSlate::TextWrapMode::NoWrap);
            }
        }

        y += track_h;
    }

    if (m_CurrentAsset->m_Tracks.empty())
    {
        r.drawText(ZSlate::UIRect(right_x + 8.0f * scale, lanes_top + 8.0f * scale, right_w - 16.0f * scale, track_h),
                   "No tracks. Use the toolbar to add a track.",
                   12.0f * scale,
                   kDimColor,
                   ZSlate::TextAnchor::MiddleLeft,
                   ZSlate::TextWrapMode::NoWrap);
    }

    // Playhead across the lanes.
    const float head_px = (m_CurrentTime - m_TimeRangeStart) * pps;
    if (head_px >= 0.0f && head_px <= right_w)
    {
        r.drawQuad(ZSlate::UIRect(right_x + head_px, region.y, 2.0f, region.height), kPlayhead);
    }
    r.popClipRect();
}

// ---------------------------------------------------------------------------
// Manual canvas mouse interaction
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::HandleCanvasMouse(const ZSlate::UIRect& region,
                                             float scale,
                                             const ZSlate::Vector2& mouse,
                                             bool over_canvas,
                                             bool clicked,
                                             bool down,
                                             bool released)
{
    if (m_CurrentAsset == nullptr)
        return;

    const float track_list_w = m_TrackListWidth * scale;
    const float ruler_h = m_TimeRulerHeight * scale;
    const float track_h = m_TrackHeight * scale;
    const float pps = m_PixelsPerSecond * scale;
    const float right_x = region.x + track_list_w;
    const float right_w = std::max(1.0f, region.width - track_list_w);
    const float lanes_top = region.y + ruler_h;

    // End an in-progress drag regardless of cursor position.
    if (m_DraggingClip && released)
    {
        m_DraggingClip = false;
        m_DragTrack = nullptr;
        m_DragClipIndex = -1;
    }

    // Continue an active drag.
    if (m_DraggingClip && down && m_DragTrack != nullptr)
    {
        const float delta_time = (mouse.x - m_DragStartMouseX) / pps;
        float new_start = m_DragStartTime + delta_time;
        if (new_start < 0.0f)
            new_start = 0.0f;
        if (m_SnapToFrames)
            new_start = FrameToTime(TimeToFrame(new_start));
        OnClipMoved(m_DragTrack, m_DragClipIndex, new_start);
        return;
    }

    if (!over_canvas || !clicked)
        return;

    const bool in_right = mouse.x >= right_x && mouse.x <= right_x + right_w;

    // Click in the ruler -> scrub the playhead.
    if (in_right && mouse.y >= region.y && mouse.y < lanes_top)
    {
        SetTime(m_TimeRangeStart + (mouse.x - right_x) / pps);
        return;
    }

    // Click in a lane -> hit-test clips.
    if (in_right && mouse.y >= lanes_top)
    {
        float y = lanes_top;
        for (size_t i = 0; i < m_CurrentAsset->m_Tracks.size(); ++i)
        {
            TimelineTrack* track = m_CurrentAsset->m_Tracks[i];
            if (track == nullptr)
                continue;

            if (mouse.y >= y && mouse.y < y + track_h)
            {
                for (size_t j = 0; j < track->m_Clips.size(); ++j)
                {
                    TimelineClip* clip = track->m_Clips[j];
                    if (clip == nullptr || !clip->m_Enabled)
                        continue;

                    const float clip_x = right_x + (clip->m_StartTime - m_TimeRangeStart) * pps;
                    const float clip_w = clip->m_Duration * pps;
                    if (mouse.x >= clip_x && mouse.x <= clip_x + clip_w)
                    {
                        SelectClip(track, static_cast<int>(j));
                        // Begin a move drag (Select/Move modes).
                        if (m_EditMode == 0 || m_EditMode == 1)
                        {
                            m_DraggingClip = true;
                            m_DragTrack = track;
                            m_DragClipIndex = static_cast<int>(j);
                            m_DragStartMouseX = mouse.x;
                            m_DragStartTime = clip->m_StartTime;
                        }
                        return;
                    }
                }
                // Clicked empty lane -> clear selection.
                ClearSelection();
                return;
            }
            y += track_h;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-frame entry point
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::OnGUI()
{
    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

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

    // Refresh live toolbar labels.
    if (m_PlayLabel)
        m_PlayLabel->Text = (m_PlayState == TimelinePlayState::Playing) ? "||" : ">";
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
    if (m_ZoomText)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f px/s", m_PixelsPerSecond);
        m_ZoomText->Text = buf;
    }
    if (m_ModeLabel)
        m_ModeLabel->Text = std::string("Mode: ") + kModeNames[m_EditMode % 4];

    // ---- Geometry -----------------------------------------------------------
    // P10c: native-host panels source their leaf rect from EditorView::NativeRect()
    // (no ImGui::Begin / item to probe); otherwise use the ImGui content region.
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    m_Toolbar->CacheDesiredSize();
    const float toolbar_h = std::min(avail.y, std::max(m_Toolbar->GetDesiredSize().y, 26.0f * ui_scale));
    m_ToolbarHeight = toolbar_h;

    const ZSlate::UIRect toolbar_region(pos.x, pos.y, avail.x, toolbar_h);
    const FGeometry toolbar_geom(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, toolbar_h));
    const ZSlate::UIRect canvas_region(pos.x, pos.y + toolbar_h, avail.x, avail.y - toolbar_h);

    // ---- Paint --------------------------------------------------------------
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;

        renderer.pushClipRect(toolbar_region, true);
        m_Toolbar->Paint(ctx, toolbar_geom);
        renderer.popClipRect();

        PaintCanvas(renderer, canvas_region, ui_scale);
    }

    // ---- Input --------------------------------------------------------------
    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed EditorSlateHost.
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, ZSlate::UIRect(pos.x, pos.y, avail.x, avail.y), ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    // Toolbar widgets first (buttons / checkboxes).
    m_Input.ProcessMouse(m_Toolbar, mouse, over_canvas, left_down, wheel);

    // Then the custom timeline canvas (only when the cursor is below the toolbar
    // so toolbar clicks are never double-handled). Press/release are native edges
    // (WasLeftPressedThisFrame + a tracked down->up transition).
    const bool in_canvas = over_canvas && mouse.y >= canvas_region.y;
    const bool clicked = in_canvas && host.WasLeftPressedThisFrame();
    const bool released = !left_down && m_PrevLeftDown;
    HandleCanvasMouse(canvas_region, ui_scale, mouse, in_canvas, clicked, left_down, released);

    // Horizontal scroll via mouse wheel over the canvas.
    if (in_canvas && wheel != 0.0f)
    {
        m_TimeRangeStart -= wheel * (1.0f / m_PixelsPerSecond) * 60.0f;
        if (m_TimeRangeStart < 0.0f)
            m_TimeRangeStart = 0.0f;
    }

    // Keyboard: Space toggles play/pause when the window is hovered/focused.
    if (over_canvas)
    {
        for (EKey key : host.GetKeysThisFrame())
        {
            if (key != EKey::Space)
                continue;
            if (m_PlayState == TimelinePlayState::Playing)
                Pause();
            else
                Play();
        }
    }

    m_PrevLeftDown = left_down;
}

void ZSlateTimelineWindow::AdvancePlayback()
{
    if (m_PlayState != TimelinePlayState::Playing || m_CurrentAsset == nullptr)
        return;

    const double now = GET_SYSTEM(WindowSystem)->GetApplication()->GetTime();
    float dt = (m_LastFrameTime > 0.0) ? static_cast<float>(now - m_LastFrameTime) : 0.0f;
    m_LastFrameTime = now;
    if (dt <= 0.0f || dt > 0.25f)
        dt = 0.016f;  // clamp first frame / hitches

    m_CurrentTime += dt * m_PlaySpeed;
    if (m_CurrentTime >= m_CurrentAsset->m_Duration)
    {
        if (m_IsLooping)
            m_CurrentTime = 0.0f;
        else
            Stop();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::SetTimelineAsset(TimelineAsset* asset)
{
    m_CurrentAsset = asset;
    ClearSelection();
    Stop();
}

void ZSlateTimelineWindow::Play()
{
    m_PlayState = TimelinePlayState::Playing;
    m_LastFrameTime = GET_SYSTEM(WindowSystem)->GetApplication()->GetTime();
}

void ZSlateTimelineWindow::Pause()
{
    m_PlayState = TimelinePlayState::Paused;
}

void ZSlateTimelineWindow::Stop()
{
    m_PlayState = TimelinePlayState::Stopped;
    m_CurrentTime = 0.0f;
}

void ZSlateTimelineWindow::SetTime(float time)
{
    m_CurrentTime = time;
    if (m_CurrentTime < 0.0f)
        m_CurrentTime = 0.0f;
    if (m_CurrentAsset != nullptr && m_CurrentTime > m_CurrentAsset->m_Duration)
        m_CurrentTime = m_CurrentAsset->m_Duration;
}

// ---------------------------------------------------------------------------
// Time / pixel mapping
// ---------------------------------------------------------------------------
int ZSlateTimelineWindow::TimeToFrame(float time) const
{
    return m_CurrentAsset == nullptr ? 0 : m_CurrentAsset->getFrameAtTime(time);
}

float ZSlateTimelineWindow::FrameToTime(int frame) const
{
    return m_CurrentAsset == nullptr ? 0.0f : m_CurrentAsset->getTimeAtFrame(frame);
}

// ---------------------------------------------------------------------------
// Data ops
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::AddTrack(const std::string& track_type)
{
    if (m_CurrentAsset == nullptr)
        return;

    TimelineTrack* new_track = nullptr;
    if (track_type == "AnimationTimelineTrack")
        new_track = MemoryManager::CreateObject<AnimationTimelineTrack>();
    else if (track_type == "ActivationTimelineTrack")
        new_track = MemoryManager::CreateObject<ActivationTimelineTrack>();
    else if (track_type == "AudioTimelineTrack")
        new_track = MemoryManager::CreateObject<AudioTimelineTrack>();
    else if (track_type == "EventTimelineTrack")
        new_track = MemoryManager::CreateObject<EventTimelineTrack>();

    if (new_track != nullptr)
    {
        new_track->m_Name = track_type + " " + std::to_string(m_CurrentAsset->m_Tracks.size() + 1);
        m_CurrentAsset->m_Tracks.push_back(new_track);
    }
}

void ZSlateTimelineWindow::DeleteTrack(int track_index)
{
    if (m_CurrentAsset == nullptr || track_index < 0 ||
        track_index >= static_cast<int>(m_CurrentAsset->m_Tracks.size()))
        return;

    m_CurrentAsset->m_Tracks.erase(m_CurrentAsset->m_Tracks.begin() + track_index);
    ClearSelection();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
void ZSlateTimelineWindow::ClearSelection()
{
    m_SelTrack = nullptr;
    m_SelClipIndex = -1;
}

void ZSlateTimelineWindow::SelectClip(TimelineTrack* track, int clip_index)
{
    if (track == nullptr || clip_index < 0 || clip_index >= static_cast<int>(track->m_Clips.size()))
    {
        ClearSelection();
        return;
    }
    m_SelTrack = track;
    m_SelClipIndex = clip_index;
}

bool ZSlateTimelineWindow::IsClipSelected(TimelineTrack* track, int clip_index) const
{
    return m_SelTrack == track && m_SelClipIndex == clip_index;
}

void ZSlateTimelineWindow::OnClipMoved(TimelineTrack* track, int clip_index, float new_start_time)
{
    if (track == nullptr || clip_index < 0 || clip_index >= static_cast<int>(track->m_Clips.size()))
        return;
    if (CanMoveClip(track, clip_index, new_start_time))
        track->m_Clips[clip_index]->m_StartTime = new_start_time;
}

bool ZSlateTimelineWindow::CanMoveClip(TimelineTrack* track, int clip_index, float new_start_time) const
{
    if (track == nullptr || clip_index < 0 || clip_index >= static_cast<int>(track->m_Clips.size()))
        return false;

    TimelineClip* clip = track->m_Clips[clip_index];
    if (clip == nullptr)
        return false;

    const float new_end = new_start_time + clip->m_Duration;
    for (size_t i = 0; i < track->m_Clips.size(); ++i)
    {
        if (i == static_cast<size_t>(clip_index))
            continue;
        TimelineClip* other = track->m_Clips[i];
        if (other == nullptr || !other->m_Enabled)
            continue;
        if (new_start_time < other->getEndTime() && new_end > other->m_StartTime)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Visuals
// ---------------------------------------------------------------------------
const char* ZSlateTimelineWindow::TrackTypeName(TimelineTrack* track) const
{
    if (dynamic_cast<AnimationTimelineTrack*>(track))
        return "Animation";
    if (dynamic_cast<ActivationTimelineTrack*>(track))
        return "Activation";
    if (dynamic_cast<AudioTimelineTrack*>(track))
        return "Audio";
    if (dynamic_cast<EventTimelineTrack*>(track))
        return "Event";
    return "Unknown";
}

ZSlate::UIColor ZSlateTimelineWindow::TrackColor(TimelineTrack* track) const
{
    if (dynamic_cast<AnimationTimelineTrack*>(track))
        return ZSlate::UIColor(0.39f, 0.59f, 1.0f, 0.39f);
    if (dynamic_cast<ActivationTimelineTrack*>(track))
        return ZSlate::UIColor(0.59f, 0.39f, 1.0f, 0.39f);
    if (dynamic_cast<AudioTimelineTrack*>(track))
        return ZSlate::UIColor(1.0f, 0.59f, 0.39f, 0.39f);
    if (dynamic_cast<EventTimelineTrack*>(track))
        return ZSlate::UIColor(0.39f, 1.0f, 0.59f, 0.39f);
    return ZSlate::UIColor(0.50f, 0.50f, 0.50f, 0.39f);
}

ZSlate::UIColor ZSlateTimelineWindow::ClipColor(TimelineClip* clip) const
{
    if (dynamic_cast<AnimationTimelineClip*>(clip))
        return ZSlate::UIColor(0.39f, 0.59f, 1.0f, 1.0f);
    if (dynamic_cast<ActivationTimelineClip*>(clip))
        return ZSlate::UIColor(0.59f, 0.39f, 1.0f, 1.0f);
    if (dynamic_cast<AudioTimelineClip*>(clip))
        return ZSlate::UIColor(1.0f, 0.59f, 0.39f, 1.0f);
    if (dynamic_cast<EventTimelineClip*>(clip))
        return ZSlate::UIColor(0.39f, 1.0f, 0.59f, 1.0f);
    return ZSlate::UIColor(0.50f, 0.50f, 0.50f, 1.0f);
}

std::string ZSlateTimelineWindow::ClipDisplayName(TimelineClip* clip) const
{
    if (auto* anim = dynamic_cast<AnimationTimelineClip*>(clip))
        return anim->m_AnimationPath.empty() ? std::string("Animation Clip") : anim->m_AnimationPath;
    if (auto* audio = dynamic_cast<AudioTimelineClip*>(clip))
        return audio->m_AudioPath.empty() ? std::string("Audio Clip") : audio->m_AudioPath;
    if (auto* evt = dynamic_cast<EventTimelineClip*>(clip))
        return evt->m_EventName.empty() ? std::string("Event Clip") : evt->m_EventName;
    return std::string("Clip");
}
