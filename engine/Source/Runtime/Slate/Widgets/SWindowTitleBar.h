#pragma once

// ----------------------------------------------------------------------------
// SWindowTitleBar / SResizeGrip / SWindowFrame
// ----------------------------------------------------------------------------
// Reusable OS-window chrome built as real ZSlate widgets (UE SWindowTitleBar /
// SWindow border analogue). Used by borderless tear-off windows
// (FloatingPanelManager) and available for any future custom-chrome window.
//
// Design goals (replaces the old hand-rolled GLFW-polling chrome):
//   * One coordinate space. The widgets are painted and hit-tested by a single
//     SlateInputRouter in the SAME pixel space as the rest of the panel, so
//     there is no ui_scale / framebuffer-ratio juggling at the call site.
//   * Capture-based clicks. Maximize / Close are real SButtons, so a click
//     fires only when press AND release land on the button (Slate rule) -- no
//     "pending press" edge accumulation, no press-time cursor snapshot hacks.
//   * The host owns the OS. Widgets never call GLFW. They report intent through
//     std::function delegates (toggle-maximize, close, caption drag, resize);
//     the host turns those into glfwSetWindowPos/Size on the main thread.
//   * Per-widget cursor (SWidget::GetCursor / FCursorReply). The grip asks for
//     a diagonal-resize cursor; everything else stays the arrow.
// ----------------------------------------------------------------------------

#include "Runtime/Slate/Widgets/SButton.h"
#include "Runtime/Slate/Widgets/SLeafWidget.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace ZSlate
{
// Which icon a window button paints. Maximize toggles to Restore once the
// window is maximized (mirrors UE's SWindowTitleBar caption buttons).
enum class EWindowGlyph
{
    Maximize,
    Restore,
    Close,
};

// Leaf icon painted as the content of a window caption button. Vector-drawn
// (drawRect / drawText) so it stays crisp at any DPI without a glyph atlas.
class SWindowButtonGlyph : public SLeafWidget
{
public:
    EWindowGlyph Glyph {EWindowGlyph::Close};
    UIColor Color {0.86f, 0.87f, 0.90f, 1.0f};
    float Thickness {1.0f};

    Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, 0.0f); }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect r = geom.ToRect();
        const float box = std::min(r.width, r.height) * 0.34f;
        const float cx = r.x + r.width * 0.5f;
        const float cy = r.y + r.height * 0.5f;
        switch (Glyph)
        {
            case EWindowGlyph::Maximize:
                ctx.Renderer->drawRect(UIRect(cx - box * 0.5f, cy - box * 0.5f, box, box), Color, Thickness);
                break;
            case EWindowGlyph::Restore:
            {
                const float off = box * 0.30f;
                ctx.Renderer->drawRect(UIRect(cx - box * 0.5f + off, cy - box * 0.5f - off, box, box), Color, Thickness);
                ctx.Renderer->drawRect(UIRect(cx - box * 0.5f - off, cy - box * 0.5f + off, box, box), Color, Thickness);
                break;
            }
            case EWindowGlyph::Close:
                ctx.Renderer->drawText(r, "x", std::max(10.0f, r.height * 0.5f), Color,
                                       TextAnchor::MiddleCenter, TextWrapMode::NoWrap, nullptr);
                break;
        }
    }
};

// The caption strip: title text on the left, maximize + close buttons on the
// right, and the rest of the strip is a draggable caption area.
class SWindowTitleBar : public SWidget
{
public:
    std::string Title;
    float FontSize {14.0f};
    float BarHeight {26.0f};  // pixels; host sets = logical * ui_scale
    UIColor BarColor {0.16f, 0.17f, 0.20f, 1.0f};
    UIColor AccentColor {0.30f, 0.55f, 0.95f, 1.0f};
    UIColor TitleColor {0.92f, 0.92f, 0.94f, 1.0f};

    // Monotonic seconds, set by the host every frame (drives double-click).
    double Now {0.0};

    // Intent delegates -- the host performs the actual OS window op.
    std::function<void()> OnToggleMaximize;
    std::function<void()> OnClose;
    std::function<void(const Vector2&)> OnCaptionDragBegin;  // arg: press pos (client px)
    std::function<void(const Vector2&)> OnCaptionDrag;       // arg: current pos (client px)
    std::function<void(const Vector2&)> OnCaptionDragEnd;    // arg: release pos (client px)
    std::function<bool()> IsMaximizedQuery;                  // drives the maximize/restore glyph

    SWindowTitleBar()
    {
        m_MaxGlyph = std::make_shared<SWindowButtonGlyph>();
        m_MaxGlyph->Glyph = EWindowGlyph::Maximize;
        m_CloseGlyph = std::make_shared<SWindowButtonGlyph>();
        m_CloseGlyph->Glyph = EWindowGlyph::Close;

        m_MaxButton = std::make_shared<SButton>();
        m_MaxButton->NormalColor = UIColor(0.0f, 0.0f, 0.0f, 0.0f);
        m_MaxButton->HoverColor = UIColor(1.0f, 1.0f, 1.0f, 0.14f);
        m_MaxButton->PressedColor = UIColor(0.0f, 0.0f, 0.0f, 0.22f);
        m_MaxButton->Padding = FMargin(0.0f);
        // Glyph fills the button (its desired size is 0; Center would collapse it).
        m_MaxButton->HAlign = EHorizontalAlignment::Fill;
        m_MaxButton->VAlign = EVerticalAlignment::Fill;
        m_MaxButton->SetContent(m_MaxGlyph);
        m_MaxButton->OnClicked = [this] { if (OnToggleMaximize) OnToggleMaximize(); };

        m_CloseButton = std::make_shared<SButton>();
        m_CloseButton->NormalColor = UIColor(0.0f, 0.0f, 0.0f, 0.0f);
        m_CloseButton->HoverColor = UIColor(0.78f, 0.22f, 0.22f, 0.92f);
        m_CloseButton->PressedColor = UIColor(0.55f, 0.15f, 0.15f, 1.0f);
        m_CloseButton->Padding = FMargin(0.0f);
        m_CloseButton->HAlign = EHorizontalAlignment::Fill;
        m_CloseButton->VAlign = EVerticalAlignment::Fill;
        m_CloseButton->SetContent(m_CloseGlyph);
        m_CloseButton->OnClicked = [this] { if (OnClose) OnClose(); };
    }

    int GetChildCount() const override { return 2; }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index == 0)
            return m_MaxButton;
        if (index == 1)
            return m_CloseButton;
        return nullptr;
    }

    Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, BarHeight); }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        const float btn = BarHeight;  // square buttons, height of the strip
        const Vector2 p = allotted.AbsolutePosition;
        const Vector2 s = allotted.LocalSize;
        out.push_back({m_MaxButton, FGeometry(Vector2(p.x + s.x - 2.0f * btn, p.y), Vector2(btn, BarHeight))});
        out.push_back({m_CloseButton, FGeometry(Vector2(p.x + s.x - btn, p.y), Vector2(btn, BarHeight))});
        // Keep the glyph in sync with the live maximize state (cheap, per-frame).
        m_MaxGlyph->Glyph = (IsMaximizedQuery && IsMaximizedQuery()) ? EWindowGlyph::Restore : EWindowGlyph::Maximize;
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        const UIRect r = geom.ToRect();
        ctx.Renderer->drawQuad(r, BarColor);
        const float accent_h = std::max(1.0f, BarHeight * 0.08f);
        ctx.Renderer->drawQuad(UIRect(r.x, r.y + r.height - accent_h, r.width, accent_h), AccentColor);
        if (!Title.empty())
        {
            const float pad = BarHeight * 0.30f;
            const float text_w = std::max(1.0f, r.width - 2.0f * BarHeight - pad);
            ctx.Renderer->drawText(UIRect(r.x + pad, r.y, text_w, r.height), Title, FontSize, TitleColor,
                                   TextAnchor::MiddleLeft, TextWrapMode::NoWrap, nullptr);
        }
    }

    // Caption area drag: only reached when the press is NOT over a button (the
    // router descends into the topmost child first, so buttons win).
    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        const bool is_double = m_LastDownTime >= 0.0 && (Now - m_LastDownTime) <= kDoubleClickSeconds &&
                               std::abs(pos.x - m_LastDownPos.x) <= kDoubleClickDist &&
                               std::abs(pos.y - m_LastDownPos.y) <= kDoubleClickDist;
        m_LastDownTime = Now;
        m_LastDownPos = pos;
        if (is_double)
        {
            m_LastDownTime = -1.0;  // consume so a triple-click doesn't re-toggle
            if (OnToggleMaximize)
                OnToggleMaximize();
            return FReply::Handled();
        }
        m_Dragging = true;
        if (OnCaptionDragBegin)
            OnCaptionDragBegin(pos);
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_Dragging && OnCaptionDrag)
            OnCaptionDrag(pos);
    }

    FReply OnMouseButtonUp(const Vector2& pos, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        if (m_Dragging)
        {
            m_Dragging = false;
            if (OnCaptionDragEnd)
                OnCaptionDragEnd(pos);
        }
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            if (OnCaptionDragEnd)
                OnCaptionDragEnd(m_LastDownPos);
        }
    }

    bool IsDragging() const { return m_Dragging; }

private:
    static constexpr double kDoubleClickSeconds = 0.30;
    static constexpr float kDoubleClickDist = 6.0f;

    std::shared_ptr<SButton> m_MaxButton;
    std::shared_ptr<SButton> m_CloseButton;
    std::shared_ptr<SWindowButtonGlyph> m_MaxGlyph;
    std::shared_ptr<SWindowButtonGlyph> m_CloseGlyph;
    bool m_Dragging {false};
    double m_LastDownTime {-1.0};
    Vector2 m_LastDownPos {0.0f, 0.0f};
};

// Bottom-right resize grip. Captures the mouse on press so the resize tracks
// even when the cursor leaves the small grip triangle.
class SResizeGrip : public SLeafWidget
{
public:
    UIColor Color {0.55f, 0.57f, 0.62f, 0.9f};
    bool Enabled {true};  // host disables while maximized

    std::function<void(const Vector2&)> OnResizeBegin;  // arg: press pos (client px)
    std::function<void(const Vector2&)> OnResize;        // arg: current pos (client px)
    std::function<void()> OnResizeEnd;

    Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, 0.0f); }

    ECursorType GetCursor() const override { return Enabled ? ECursorType::ResizeNWSE : ECursorType::Default; }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr || !Enabled)
            return;
        const UIRect r = geom.ToRect();
        const Vector2 tri[3] = {
            Vector2(r.x + r.width, r.y),
            Vector2(r.x + r.width, r.y + r.height),
            Vector2(r.x, r.y + r.height),
        };
        ctx.Renderer->drawConvexPoly(tri, 3, Color);
    }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0 || !Enabled)
            return FReply::Unhandled();
        m_Resizing = true;
        if (OnResizeBegin)
            OnResizeBegin(pos);
        return FReply::Handled().CaptureMouse(this);
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (m_Resizing && OnResize)
            OnResize(pos);
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        EndResize();
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override { EndResize(); }

private:
    void EndResize()
    {
        if (!m_Resizing)
            return;
        m_Resizing = false;
        if (OnResizeEnd)
            OnResizeEnd();
    }

    bool m_Resizing {false};
};

// Composite chrome root: title bar across the top + resize grip at the
// bottom-right, with the window body left empty for the panel content (which
// the host paints + routes separately). A single SlateInputRouter over this
// frame drives the whole chrome in one pass.
class SWindowFrame : public SWidget
{
public:
    std::shared_ptr<SWindowTitleBar> TitleBar;
    std::shared_ptr<SResizeGrip> Grip;
    float BarHeight {26.0f};
    float GripSize {16.0f};

    SWindowFrame()
    {
        TitleBar = std::make_shared<SWindowTitleBar>();
        Grip = std::make_shared<SResizeGrip>();
    }

    int GetChildCount() const override { return 2; }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index == 0)
            return TitleBar;
        if (index == 1)
            return Grip;
        return nullptr;
    }

    Vector2 ComputeDesiredSize() const override { return Vector2(0.0f, 0.0f); }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        const Vector2 p = allotted.AbsolutePosition;
        const Vector2 s = allotted.LocalSize;
        TitleBar->BarHeight = BarHeight;
        out.push_back({TitleBar, FGeometry(p, Vector2(s.x, BarHeight))});
        out.push_back({Grip, FGeometry(Vector2(p.x + s.x - GripSize, p.y + s.y - GripSize),
                                       Vector2(GripSize, GripSize))});
    }
};
}  // namespace ZSlate
