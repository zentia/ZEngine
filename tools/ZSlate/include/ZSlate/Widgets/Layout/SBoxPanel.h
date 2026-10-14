#pragma once

// ============================================================================
// ZSlate/Widgets/Layout/SBoxPanel.h
//
// Linear stack panel. Children are laid out along the panel's main axis using
// their desired size; children whose WantsFill*() returns true share whatever
// space is left over. The cross axis is always stretched to the panel size.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

#include <memory>
#include <vector>

namespace ZSlate
{

class SBoxPanel : public SWidget
{
public:
    explicit SBoxPanel(EOrientation orientation) : m_Orientation(orientation) {}

    EOrientation GetOrientation() const { return m_Orientation; }

    void AddSlot(const std::shared_ptr<SWidget>& child)
    {
        if (!child) return;
        child->SetParent(this);
        m_Children.push_back(child);
    }

    void ClearChildren() { m_Children.clear(); }

    const std::vector<std::shared_ptr<SWidget>>& GetChildren() const { return m_Children; }

    Vector2 ComputeDesiredSize() const override;

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override;

protected:
    EOrientation                          m_Orientation = EOrientation::Vertical;
    std::vector<std::shared_ptr<SWidget>> m_Children;
};

class SHorizontalBox : public SBoxPanel
{
public:
    SHorizontalBox() : SBoxPanel(EOrientation::Horizontal) {}
};

class SVerticalBox : public SBoxPanel
{
public:
    SVerticalBox() : SBoxPanel(EOrientation::Vertical) {}
};

} // namespace ZSlate
