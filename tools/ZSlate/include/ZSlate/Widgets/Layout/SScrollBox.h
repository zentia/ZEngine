#pragma once

// ============================================================================
// ZSlate/Widgets/Layout/SScrollBox.h
//
// Single-child scrolling container. Scrolling is driven by the mouse wheel
// and clamped to the child's desired size.
// ============================================================================

#include "../SWidget.h"
#include "../../Core/ZSlateTypes.h"

#include <memory>

namespace ZSlate
{

class SScrollBox : public SWidget
{
public:
    void SetContent(const std::shared_ptr<SWidget>& content)
    {
        m_Content = content;
        if (m_Content) m_Content->SetParent(this);
    }

    const std::shared_ptr<SWidget>& GetContent() const { return m_Content; }

    // Backwards-compatible slot helper.
    void AddSlot(const std::shared_ptr<SWidget>& content) { SetContent(content); }

    bool WantsFillHeight() const override { return true; }

    Vector2 ComputeDesiredSize() const override;

    void OnArrangeChildren(const FGeometry& allottedGeometry,
                           std::vector<FArrangedChild>& outChildren) const override;

    FReply OnMouseWheel(const Vector2& absolutePos, float delta) override;

    float GetScrollOffset() const { return m_ScrollOffset; }
    void  SetScrollOffset(float offset) { m_ScrollOffset = offset; }

private:
    std::shared_ptr<SWidget> m_Content;
    mutable float            m_ScrollOffset = 0.0f;
};

} // namespace ZSlate
