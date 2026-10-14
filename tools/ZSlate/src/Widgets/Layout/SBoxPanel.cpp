#include "ZSlate/Widgets/Layout/SBoxPanel.h"

#include <algorithm>

namespace ZSlate
{

Vector2 SBoxPanel::ComputeDesiredSize() const
{
    Vector2 result(0.0f, 0.0f);
    for (const auto& child : m_Children)
    {
        if (!child || !child->IsVisible()) continue;
        const Vector2 desired = child->ComputeDesiredSize();
        if (m_Orientation == EOrientation::Vertical)
        {
            result.x = std::max(result.x, desired.x);
            result.y += desired.y;
        }
        else
        {
            result.x += desired.x;
            result.y = std::max(result.y, desired.y);
        }
    }
    return result;
}

void SBoxPanel::OnArrangeChildren(const FGeometry& allottedGeometry,
                                  std::vector<FArrangedChild>& outChildren) const
{
    const bool   vertical = (m_Orientation == EOrientation::Vertical);
    const Vector2 size    = allottedGeometry.LocalSize;

    struct FEntry
    {
        std::shared_ptr<SWidget> Widget;
        float                    Along = 0.0f;
        bool                     Fill  = false;
    };

    std::vector<FEntry> entries;
    entries.reserve(m_Children.size());

    float fixedSum  = 0.0f;
    int   fillCount = 0;

    for (const auto& child : m_Children)
    {
        if (!child || !child->IsVisible()) continue;

        const Vector2 desired = child->ComputeDesiredSize();
        const bool    fill    = vertical ? child->WantsFillHeight() : child->WantsFillWidth();
        const float   along   = vertical ? desired.y : desired.x;

        entries.push_back({ child, along, fill });
        if (fill) ++fillCount;
        else      fixedSum += along;
    }

    const float available = vertical ? size.y : size.x;
    const float extra     = std::max(0.0f, available - fixedSum);
    const float perFill   = fillCount > 0 ? extra / static_cast<float>(fillCount) : 0.0f;

    float offset = 0.0f;
    for (const FEntry& entry : entries)
    {
        const float along = entry.Along + (entry.Fill ? perFill : 0.0f);
        if (vertical)
        {
            outChildren.emplace_back(entry.Widget,
                allottedGeometry.MakeChild(Vector2(0.0f, offset),
                                           Vector2(size.x, along)));
        }
        else
        {
            outChildren.emplace_back(entry.Widget,
                allottedGeometry.MakeChild(Vector2(offset, 0.0f),
                                           Vector2(along, size.y)));
        }
        offset += along;
    }
}

} // namespace ZSlate
