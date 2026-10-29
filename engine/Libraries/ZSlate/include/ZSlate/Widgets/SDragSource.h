#pragma once

#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/SCompoundWidget.h"

#include <functional>
#include <memory>

namespace ZSlate
{
// The mirror image of SDropTarget: an invisible wrapper that turns its content
// into a drag SOURCE. Return a non-null operation from OnDragDetectedHandler to
// begin the gesture; the router then owns the op and clears the process-wide
// channel when the gesture ends (a hand-painted source has to do that itself --
// see ZSlateAnimatorWindow::EndAssetDragOut).
//
// OnDragDetected is polled over the whole press path, innermost first, so this
// keeps working when the content swallows the press (a text box starting a text
// selection): the wrapper is still asked, and returning a non-null op releases
// whatever capture the inner widget took.
class SDragSource : public SCompoundWidget
{
public:
    // Return a non-null operation to start a drag, or null to opt out this frame
    // (e.g. the row has no value to carry).
    std::function<std::shared_ptr<FDragDropOperation>(const Vector2& screen_pos)> OnDragDetectedHandler;

    std::shared_ptr<FDragDropOperation> OnDragDetected(const Vector2& screen_pos) override
    {
        return OnDragDetectedHandler ? OnDragDetectedHandler(screen_pos) : nullptr;
    }
};
}  // namespace ZSlate
