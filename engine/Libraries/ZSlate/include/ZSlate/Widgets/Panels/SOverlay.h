#pragma once

#include "ZSlate/Widgets/SWidget.h"

#include <memory>
#include <vector>

namespace ZSlate
{
// Z-stack container (UE Slate SOverlay analogue): children are painted in add
// order (later children on top), each occupying the full allotted geometry,
// aligned + padded within it. Used as the UMG viewport root so multiple
// UserWidgets stack by z-order.
class SOverlay : public SWidget
{
public:
    struct FSlot
    {
        std::shared_ptr<SWidget> Widget;
        FMargin Padding;
        EHorizontalAlignment HAlign {EHorizontalAlignment::Fill};
        EVerticalAlignment VAlign {EVerticalAlignment::Fill};

        FSlot& SetPadding(const FMargin& padding)
        {
            Padding = padding;
            return *this;
        }
        FSlot& SetHAlign(EHorizontalAlignment align)
        {
            HAlign = align;
            return *this;
        }
        FSlot& SetVAlign(EVerticalAlignment align)
        {
            VAlign = align;
            return *this;
        }
    };

    FSlot& AddSlot(std::shared_ptr<SWidget> widget)
    {
        auto slot = std::make_unique<FSlot>();
        slot->Widget = std::move(widget);
        m_Slots.push_back(std::move(slot));
        return *m_Slots.back();
    }

    void RemoveChild(const std::shared_ptr<SWidget>& widget)
    {
        for (auto it = m_Slots.begin(); it != m_Slots.end(); ++it)
        {
            if ((*it)->Widget == widget)
            {
                m_Slots.erase(it);
                return;
            }
        }
    }

    void ClearChildren() { m_Slots.clear(); }

    int GetChildCount() const override { return static_cast<int>(m_Slots.size()); }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index < 0 || index >= static_cast<int>(m_Slots.size()))
            return nullptr;
        return m_Slots[static_cast<size_t>(index)]->Widget;
    }

    Vector2 ComputeDesiredSize() const override
    {
        Vector2 desired(0.0f, 0.0f);
        for (const auto& slot : m_Slots)
        {
            if (!slot->Widget || slot->Widget->Visibility == EVisibility::Collapsed)
                continue;
            const Vector2 child = slot->Widget->GetDesiredSize();
            const float w = child.x + slot->Padding.GetTotalHorizontal();
            const float h = child.y + slot->Padding.GetTotalVertical();
            if (w > desired.x)
                desired.x = w;
            if (h > desired.y)
                desired.y = h;
        }
        return desired;
    }

    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override
    {
        for (const auto& slot : m_Slots)
        {
            if (!slot->Widget || slot->Widget->Visibility == EVisibility::Collapsed)
                continue;
            const float region_x = allotted.AbsolutePosition.x + slot->Padding.Left;
            const float region_y = allotted.AbsolutePosition.y + slot->Padding.Top;
            const float region_w = allotted.LocalSize.x - slot->Padding.GetTotalHorizontal();
            const float region_h = allotted.LocalSize.y - slot->Padding.GetTotalVertical();
            out.push_back({slot->Widget,
                           AlignChildInRegion(region_x, region_y, region_w, region_h,
                                              slot->Widget->GetDesiredSize(), slot->HAlign, slot->VAlign)});
        }
    }

private:
    std::vector<std::unique_ptr<FSlot>> m_Slots;
};
}  // namespace ZSlate
