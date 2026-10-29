#include "ZSlate/Widgets/Layout/SBoxPanel.h"

namespace ZSlate
{
Vector2 SBoxPanel::ComputeDesiredSize() const
{
    // Main axis = sum of children (+ their padding); Cross axis = max of children.
    float main_axis = 0.0f;
    float cross_axis = 0.0f;

    for (const std::unique_ptr<FSlot>& slot : m_Slots)
    {
        if (!slot->Widget || slot->Widget->Visibility == EVisibility::Collapsed)
            continue;

        const Vector2 child = slot->Widget->GetDesiredSize();
        const float child_w = child.x + slot->Padding.GetTotalHorizontal();
        const float child_h = child.y + slot->Padding.GetTotalVertical();

        if (Orientation == EOrientation::Horizontal)
        {
            main_axis += child_w;
            if (child_h > cross_axis)
                cross_axis = child_h;
        }
        else
        {
            main_axis += child_h;
            if (child_w > cross_axis)
                cross_axis = child_w;
        }
    }

    return (Orientation == EOrientation::Horizontal) ? Vector2(main_axis, cross_axis)
                                                     : Vector2(cross_axis, main_axis);
}

void SBoxPanel::ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const
{
    const bool horizontal = (Orientation == EOrientation::Horizontal);
    const float available_main = horizontal ? allotted.LocalSize.x : allotted.LocalSize.y;

    // Pass 1: reserve space for Auto slots, accumulate Stretch weights.
    float auto_consumed = 0.0f;
    float total_fill_weight = 0.0f;
    for (const std::unique_ptr<FSlot>& slot : m_Slots)
    {
        if (!slot->Widget || slot->Widget->Visibility == EVisibility::Collapsed)
            continue;

        const Vector2 child = slot->Widget->GetDesiredSize();
        if (slot->SizeRule == ESizeRule::Auto)
        {
            const float main_extent =
                (horizontal ? child.x + slot->Padding.GetTotalHorizontal()
                            : child.y + slot->Padding.GetTotalVertical());
            auto_consumed += main_extent;
        }
        else
        {
            auto_consumed += horizontal ? slot->Padding.GetTotalHorizontal() : slot->Padding.GetTotalVertical();
            total_fill_weight += (slot->FillSize > 0.0f) ? slot->FillSize : 0.0f;
        }
    }

    float remaining_for_fill = available_main - auto_consumed;
    if (remaining_for_fill < 0.0f)
        remaining_for_fill = 0.0f;

    // Pass 2: lay out left-to-right / top-to-bottom.
    float cursor = horizontal ? allotted.AbsolutePosition.x : allotted.AbsolutePosition.y;
    for (const std::unique_ptr<FSlot>& slot : m_Slots)
    {
        if (!slot->Widget || slot->Widget->Visibility == EVisibility::Collapsed)
            continue;

        const Vector2 child = slot->Widget->GetDesiredSize();

        float slot_main = 0.0f;
        if (slot->SizeRule == ESizeRule::Auto)
        {
            slot_main = (horizontal ? child.x + slot->Padding.GetTotalHorizontal()
                                    : child.y + slot->Padding.GetTotalVertical());
        }
        else
        {
            const float share = (total_fill_weight > 0.0f)
                                    ? remaining_for_fill * (slot->FillSize / total_fill_weight)
                                    : 0.0f;
            slot_main = share + (horizontal ? slot->Padding.GetTotalHorizontal()
                                            : slot->Padding.GetTotalVertical());
        }

        // The slot's cross-axis extent always fills the panel cross size.
        float region_x;
        float region_y;
        float region_w;
        float region_h;
        if (horizontal)
        {
            region_x = cursor + slot->Padding.Left;
            region_y = allotted.AbsolutePosition.y + slot->Padding.Top;
            region_w = slot_main - slot->Padding.GetTotalHorizontal();
            region_h = allotted.LocalSize.y - slot->Padding.GetTotalVertical();
        }
        else
        {
            region_x = allotted.AbsolutePosition.x + slot->Padding.Left;
            region_y = cursor + slot->Padding.Top;
            region_w = allotted.LocalSize.x - slot->Padding.GetTotalHorizontal();
            region_h = slot_main - slot->Padding.GetTotalVertical();
        }

        if (region_w < 0.0f)
            region_w = 0.0f;
        if (region_h < 0.0f)
            region_h = 0.0f;

        const FGeometry child_geom =
            AlignChildInRegion(region_x, region_y, region_w, region_h, child, slot->HAlign, slot->VAlign);
        out.push_back({slot->Widget, child_geom});

        cursor += slot_main;
    }
}
}  // namespace ZSlate
