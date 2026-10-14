#include "ZSlate/Widgets/SWidget.h"

namespace ZSlate
{

void PaintWidget(const std::shared_ptr<SWidget>& widget,
                 const FPaintContext& context,
                 const FGeometry& geometry)
{
    if (!widget || !widget->IsVisible()) return;

    widget->SetCachedGeometry(geometry);
    widget->OnPaint(context, geometry);

    std::vector<FArrangedChild> children;
    widget->OnArrangeChildren(geometry, children);

    for (const FArrangedChild& child : children)
    {
        PaintWidget(child.Widget, context, child.Geometry);
    }
}

} // namespace ZSlate
