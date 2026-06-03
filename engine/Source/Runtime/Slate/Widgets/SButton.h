#pragma once

#include "Runtime/Slate/Application/SlateDragDrop.h"
#include "Runtime/Slate/Widgets/SCompoundWidget.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <functional>
#include <memory>

namespace ZSlate
{
// A clickable button wrapping one content child (usually an STextBlock).
// NOTE: input routing (hover/press/click dispatch) lands in P3. For now the
// visual state fields exist and OnClicked is stored, but state is only updated
// once the event system is wired.
class SButton : public SCompoundWidget
{
public:
    UIColor NormalColor {0.20f, 0.20f, 0.22f, 1.0f};
    UIColor HoverColor {0.28f, 0.28f, 0.32f, 1.0f};
    UIColor PressedColor {0.15f, 0.15f, 0.17f, 1.0f};
    // Outline drawn over the button while a compatible drag hovers it.
    UIColor DropHighlightColor {0.30f, 0.55f, 0.95f, 1.0f};

    std::function<void()> OnClicked;
    // Fired on right-button release back over the button (screen-space position,
    // handy for anchoring a context menu).
    std::function<void(const Vector2&)> OnRightClicked;

    // --- Drag and drop hooks (all optional) ---------------------------------
    // Return a non-null operation to make this button a drag SOURCE.
    std::function<std::shared_ptr<FDragDropOperation>(const Vector2&)> OnDragDetectedHandler;
    // Return true to accept `op` as a drop TARGET (drives the highlight + drop).
    std::function<bool(const std::shared_ptr<FDragDropOperation>&)> CanAcceptDrop;
    // Called when an accepted drop is released over this button.
    std::function<void(const std::shared_ptr<FDragDropOperation>&)> OnDropHandler;

    SButton()
    {
        Padding = FMargin(8.0f, 4.0f);
        HAlign = EHorizontalAlignment::Center;
        VAlign = EVerticalAlignment::Center;
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;

        UIColor color = NormalColor;
        if (m_Pressed)
            color = PressedColor;
        else if (m_Hovered)
            color = HoverColor;

        ctx.Renderer->drawQuad(geom.ToRect(), color);

        if (m_DropHovered)
            ctx.Renderer->drawRect(geom.ToRect(), DropHighlightColor, 2.0f);
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }
    void OnMouseCaptureLost() override
    {
        m_Pressed = false;
        m_RightPressed = false;
    }

    FReply OnMouseButtonDown(const Vector2& /*screen_pos*/, int button) override
    {
        if (button == 1)
        {
            // Only claim the right-button gesture if we have a handler for it.
            if (!OnRightClicked)
                return FReply::Unhandled();
            m_RightPressed = true;
            return FReply::Handled().CaptureMouse(this);
        }
        if (button != 0)
            return FReply::Unhandled();
        m_Pressed = true;
        return FReply::Handled().CaptureMouse(this);
    }

    FReply OnMouseButtonUp(const Vector2& screen_pos, int button) override
    {
        if (button == 1)
        {
            const bool was_pressed = m_RightPressed;
            m_RightPressed = false;
            if (was_pressed && m_CachedGeometry.IsUnderLocation(screen_pos) && OnRightClicked)
                OnRightClicked(screen_pos);
            return FReply::Handled().ReleaseMouseCapture();
        }
        if (button != 0)
            return FReply::Unhandled();
        const bool was_pressed = m_Pressed;
        m_Pressed = false;
        // Fire OnClicked only if the release lands back on the button (Slate rule).
        if (was_pressed && m_CachedGeometry.IsUnderLocation(screen_pos) && OnClicked)
            OnClicked();
        return FReply::Handled().ReleaseMouseCapture();
    }

    std::shared_ptr<FDragDropOperation> OnDragDetected(const Vector2& screen_pos) override
    {
        return OnDragDetectedHandler ? OnDragDetectedHandler(screen_pos) : nullptr;
    }

    FReply OnDragOver(const Vector2& /*screen_pos*/, const std::shared_ptr<FDragDropOperation>& op) override
    {
        return (CanAcceptDrop && CanAcceptDrop(op)) ? FReply::Handled() : FReply::Unhandled();
    }

    void OnDragEnter(const std::shared_ptr<FDragDropOperation>& /*op*/) override { m_DropHovered = true; }
    void OnDragLeave(const std::shared_ptr<FDragDropOperation>& /*op*/) override { m_DropHovered = false; }

    FReply OnDrop(const Vector2& /*screen_pos*/, const std::shared_ptr<FDragDropOperation>& op) override
    {
        m_DropHovered = false;
        if (CanAcceptDrop && CanAcceptDrop(op) && OnDropHandler)
        {
            OnDropHandler(op);
            return FReply::Handled();
        }
        return FReply::Unhandled();
    }

    bool m_Hovered {false};
    bool m_Pressed {false};
    bool m_RightPressed {false};
    bool m_DropHovered {false};
};
}  // namespace ZSlate
