#pragma once

#include "ZSlate/Widgets/SWidget.h"

#include <memory>
#include <vector>

namespace ZSlate
{
// Stacks children along one axis (UE Slate SBoxPanel / SHorizontalBox /
// SVerticalBox analogue). Each slot is either Auto (sized to the child's desired
// extent) or Stretch (shares the leftover main-axis space, weighted by FillSize).
class SBoxPanel : public SWidget
{
public:
    struct FSlot
    {
        std::shared_ptr<SWidget> Widget;
        ESizeRule SizeRule {ESizeRule::Auto};
        float FillSize {1.0f};  // only meaningful when SizeRule == Stretch
        FMargin Padding;
        EHorizontalAlignment HAlign {EHorizontalAlignment::Fill};
        EVerticalAlignment VAlign {EVerticalAlignment::Fill};

        // Fluent configuration helpers.
        FSlot& AutoSize()
        {
            SizeRule = ESizeRule::Auto;
            return *this;
        }
        FSlot& Fill(float weight = 1.0f)
        {
            SizeRule = ESizeRule::Stretch;
            FillSize = weight;
            return *this;
        }
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

    explicit SBoxPanel(EOrientation orientation)
        : Orientation(orientation) {}

    EOrientation Orientation;

    // Append a slot and return it for fluent configuration. The slot is stored in
    // a unique_ptr so the returned reference stays valid as the vector grows.
    FSlot& AddSlot(std::shared_ptr<SWidget> widget)
    {
        auto slot = std::make_unique<FSlot>();
        slot->Widget = std::move(widget);
        m_Slots.push_back(std::move(slot));
        return *m_Slots.back();
    }

    void ClearChildren() { m_Slots.clear(); }

    int GetChildCount() const override { return static_cast<int>(m_Slots.size()); }
    std::shared_ptr<SWidget> GetChildAt(int index) const override
    {
        if (index < 0 || index >= static_cast<int>(m_Slots.size()))
            return nullptr;
        return m_Slots[static_cast<size_t>(index)]->Widget;
    }

    Vector2 ComputeDesiredSize() const override;
    void ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const override;

protected:
    std::vector<std::unique_ptr<FSlot>> m_Slots;
};

class SVerticalBox : public SBoxPanel
{
public:
    SVerticalBox()
        : SBoxPanel(EOrientation::Vertical) {}
};

class SHorizontalBox : public SBoxPanel
{
public:
    SHorizontalBox()
        : SBoxPanel(EOrientation::Horizontal) {}
};
}  // namespace ZSlate
