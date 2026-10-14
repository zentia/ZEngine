#include "ZSlate/Widgets/Layout/SScrollBox.h"

#include <algorithm>

namespace ZSlate
{

Vector2 SScrollBox::ComputeDesiredSize() const
{
    if (m_Content) return m_Content->ComputeDesiredSize();
    return Vector2(0.0f, 0.0f);
}

void SScrollBox::OnArrangeChildren(const FGeometry& allottedGeometry,
                                   std::vector<FArrangedChild>& outChildren) const
{
    if (!m_Content) return;

    const Vector2 desired = m_Content->ComputeDesiredSize();
    const float   height  = std::max(desired.y, allottedGeometry.LocalSize.y);
    const float   maxScroll = std::max(0.0f, height - allottedGeometry.LocalSize.y);

    m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, maxScroll);

    outChildren.emplace_back(m_Content,
        allottedGeometry.MakeChild(Vector2(0.0f, -m_ScrollOffset),
                                   Vector2(allottedGeometry.LocalSize.x, height)));
}

FReply SScrollBox::OnMouseWheel(const Vector2& absolutePos, float delta)
{
    (void)absolutePos;
    m_ScrollOffset -= delta * 32.0f;
    if (m_ScrollOffset < 0.0f) m_ScrollOffset = 0.0f;
    return FReply::Handled();
}

} // namespace ZSlate
