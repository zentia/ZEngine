#pragma once

#include "ZSlate/Widgets/SPanel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace ZSlate
{

// =============================================================================
// SCanvas — free-form absolute positioning (UE SCanvas analogue)
// =============================================================================
//
// Children are placed at explicit (x, y) positions with optional size
// and Z-order control.
//
class SCanvas : public SPanel
{
public:
    struct FSlot : public FSlotBase
    {
        float PositionX {0.0f};
        float PositionY {0.0f};
        float SizeX {0.0f};   // 0 = auto-size from child's desired
        float SizeY {0.0f};
        int32_t ZOrder {0};
    };

    FSlot& AddSlot()
    {
        m_CanvasSlots.emplace_back();
        m_SlotsDirty = true;
        return m_CanvasSlots.back();
    }

    FSlot& AddSlot(std::shared_ptr<SWidget> widget, float x, float y)
    {
        auto& s = AddSlot();
        s.Content = std::move(widget);
        s.PositionX = x;
        s.PositionY = y;
        return s;
    }

    void ClearCanvas() { m_CanvasSlots.clear(); m_SlotsDirty = true; }

    int32_t GetNumChildren() const { return static_cast<int32_t>(m_CanvasSlots.size()); }
    std::shared_ptr<SWidget> GetChildAt(int32_t index) const override
    {
        return (index >= 0 && index < static_cast<int32_t>(m_CanvasSlots.size()))
            ? m_CanvasSlots[index].Content : nullptr;
    }

    Vector2 ComputeDesiredSize() const override
    {
        float maxX = 0, maxY = 0;
        for (const auto& s : m_CanvasSlots)
        {
            if (!s.Content || !s.Content->IsVisible()) continue;
            Vector2 child = s.Content->ComputeDesiredSize();
            maxX = std::max(maxX, s.PositionX + (s.SizeX > 0 ? s.SizeX : child.x));
            maxY = std::max(maxY, s.PositionY + (s.SizeY > 0 ? s.SizeY : child.y));
        }
        return Vector2(maxX, maxY);
    }

    void OnArrangeChildren(const FGeometry& AllottedGeometry,
                           std::vector<FArrangedChild>& OutArrangedChildren) const override
    {
        RebuildSorted();
        const UIRect view = AllottedGeometry.ToRect();

        for (const auto& s : m_SortedSlots)
        {
            if (!s.Content || !s.Content->IsVisible()) continue;
            Vector2 child = s.Content->GetDesiredSize();
            float cw = s.SizeX > 0 ? s.SizeX : child.x;
            float ch = s.SizeY > 0 ? s.SizeY : child.y;

            FGeometry childGeom = AllottedGeometry.MakeChild(
                Vector2(s.PositionX, s.PositionY), Vector2(cw, ch));
            OutArrangedChildren.emplace_back(s.Content, childGeom);
        }
    }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;
        RebuildSorted();

        for (const auto& s : m_SortedSlots)
        {
            if (!s.Content || !s.Content->IsVisible()) continue;
            Vector2 child = s.Content->GetDesiredSize();
            float cw = s.SizeX > 0 ? s.SizeX : child.x;
            float ch = s.SizeY > 0 ? s.SizeY : child.y;

            FGeometry childGeom = geom.MakeChild(
                Vector2(s.PositionX, s.PositionY), Vector2(cw, ch));
            s.Content->Paint(ctx, childGeom);
        }
    }

private:
    std::vector<FSlot> m_CanvasSlots;
    mutable std::vector<const FSlot*> m_SortedSlots;
    mutable bool m_SlotsDirty {true};

    void RebuildSorted() const
    {
        if (!m_SlotsDirty) return;
        m_SortedSlots.clear();
        for (const auto& s : m_CanvasSlots)
            m_SortedSlots.push_back(&s);
        std::sort(m_SortedSlots.begin(), m_SortedSlots.end(),
            [](const FSlot* a, const FSlot* b) { return a->ZOrder < b->ZOrder; });
        m_SlotsDirty = false;
    }
};

}  // namespace ZSlate
