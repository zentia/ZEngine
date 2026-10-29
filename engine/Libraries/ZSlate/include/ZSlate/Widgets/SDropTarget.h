#pragma once

#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <memory>

namespace ZSlate
{
// An invisible wrapper that turns its whole area into a drag-drop DROP target
// without intercepting clicks (UE Slate SDropTarget analogue). Place it as an
// ancestor of finer-grained targets: because OnDragOver bubbles deepest -> root,
// a deeper widget that accepts the payload wins, and this catch-all only fires
// when the drop lands on the "blank" area around the inner widgets.
//
// Typical use: wrap a list/panel so dropping onto empty space performs a
// container-level action (e.g. reparent-to-root) while dropping onto a row
// reparents under that row.
class SDropTarget : public SCompoundWidget
{
public:
    // Outline drawn over the whole area while a compatible drag hovers it.
    UIColor DropHighlightColor {0.30f, 0.55f, 0.95f, 0.85f};
    bool DrawHighlight {true};

    // Return true to accept `op` (drives the highlight + receives OnDrop).
    std::function<bool(const std::shared_ptr<FDragDropOperation>&)> CanAcceptDrop;
    // Called when an accepted drop is released over this area.
    std::function<void(const std::shared_ptr<FDragDropOperation>&)> OnDropHandler;

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (DrawHighlight && m_DropHovered && ctx.Renderer != nullptr)
            ctx.Renderer->DrawRect(geom.ToRect(), DropHighlightColor, 2.0f);
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

    bool m_DropHovered {false};
};
}  // namespace ZSlate
