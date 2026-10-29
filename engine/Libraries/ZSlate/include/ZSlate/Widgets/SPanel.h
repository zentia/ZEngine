#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Core/SlateEnums.h"
#include "ZSlate/Core/SlateGeometry.h"

#include <memory>
#include <vector>

namespace ZSlate
{

// =============================================================================
// FSlotBase — base slot for panel layout (UE TSlotBase analogue)
// =============================================================================

class SPanel;

struct FSlotBase
{
    virtual ~FSlotBase() = default;

    // Which panel owns this slot
    SPanel* Owner {nullptr};

    // The widget in this slot
    std::shared_ptr<SWidget> Content;

    // Layout constraints
    EHorizontalAlignment HAlign {EHorizontalAlignment::Fill};
    EVerticalAlignment   VAlign {EVerticalAlignment::Fill};

    // Main-axis size rule (used by SBoxPanel)
    ESizeRule SizeRule {ESizeRule::Auto};
    float SizeValue {0.0f};  // fixed size when SizeRule == Auto, or stretch weight

    // Margin around the widget (UE slot padding)
    FMargin SlotPadding;
};

// =============================================================================
// FArrangedChild — a child with its computed geometry (UE equivalent)
// =============================================================================

struct FArrangedChild
{
    std::shared_ptr<SWidget> Widget;
    FGeometry Geometry;

    FArrangedChild() = default;
    FArrangedChild(std::shared_ptr<SWidget> InWidget, const FGeometry& InGeometry)
        : Widget(std::move(InWidget)), Geometry(InGeometry) {}
};

// =============================================================================
// SPanel — base class for all panel widgets (UE SPanel analogue)
// =============================================================================
//
// SPanel formalises the contract: a panel has an ordered list of slot+child
// pairs, implements OnArrangeChildren to produce FArrangedChild geometries,
// and delegates painting to PaintArrangedChildren.
//
// Derived classes:
//   SBoxPanel   — horizontal/vertical box
//   SOverlay    — z-ordered stack
//   SUniformGridPanel — regular grid
//   SWrapBox    — flow layout
//
class SPanel : public SWidget
{
public:
    // --- Child management ----------------------------------------------------

    // Number of slots (children) in this panel
    int32_t GetNumChildren() const { return static_cast<int32_t>(m_Slots.size()); }

    // Add a slot. Returns the slot so the caller can configure it inline:
    //   panel->AddSlot()[.HAlign(EHorizontalAlignment::Center).Padding(4.0f)] = widget;
    FSlotBase& AddSlot();

    // Remove the last slot
    void RemoveSlot(int32_t index);

    // Clear all slots
    void ClearSlots();

    // Direct child access (for hit-testing and iteration)
    std::shared_ptr<SWidget> GetChildAt(int32_t index) const;
    int32_t GetChildCount() const override { return GetNumChildren(); }

    // --- Layout --------------------------------------------------------------

    // Derived panels override this to compute geometries for each child.
    // The default implementation returns children at identity geometry
    // (no layout — useful for pre-layout caching).
    virtual void OnArrangeChildren(const FGeometry& AllottedGeometry,
                                   std::vector<FArrangedChild>& OutArrangedChildren) const;

    // --- Paint ---------------------------------------------------------------

    // Paint method: panels override OnPaint to call PaintArrangedChildren
    // after drawing any background/border.
    void PaintArrangedChildren(const FPaintContext& ctx,
                               const std::vector<FArrangedChild>& arranged) const;

protected:
    std::vector<FSlotBase> m_Slots;
};

// =============================================================================
// Inline implementations
// =============================================================================

inline FSlotBase& SPanel::AddSlot()
{
    FSlotBase slot;
    slot.Owner = this;
    m_Slots.push_back(std::move(slot));
    return m_Slots.back();
}

inline void SPanel::RemoveSlot(int32_t index)
{
    if (index >= 0 && index < static_cast<int32_t>(m_Slots.size()))
        m_Slots.erase(m_Slots.begin() + index);
}

inline void SPanel::ClearSlots()
{
    m_Slots.clear();
}

inline std::shared_ptr<SWidget> SPanel::GetChildAt(int32_t index) const
{
    if (index >= 0 && index < static_cast<int32_t>(m_Slots.size()))
        return m_Slots[index].Content;
    return nullptr;
}

inline void SPanel::OnArrangeChildren(const FGeometry& AllottedGeometry,
                                       std::vector<FArrangedChild>& OutArrangedChildren) const
{
    for (const auto& slot : m_Slots)
    {
        if (slot.Content && slot.Content->IsVisible())
            OutArrangedChildren.emplace_back(slot.Content, AllottedGeometry);
    }
}

inline void SPanel::PaintArrangedChildren(const FPaintContext& ctx,
                                           const std::vector<FArrangedChild>& arranged) const
{
    for (const auto& child : arranged)
    {
        if (child.Widget && child.Widget->IsVisible())
            child.Widget->Paint(ctx, child.Geometry);
    }
}

}  // namespace ZSlate
