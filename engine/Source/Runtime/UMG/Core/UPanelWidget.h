#pragma once

#include "Runtime/UMG/Core/UWidget.h"

#include <memory>
#include <vector>

namespace ZUMG
{
// Base for widgets that contain child UWidgets (UE UPanelWidget analogue).
// Concrete panels decide how children map onto their ZSlate panel's slots in
// RebuildWidget(). Single-content panels (UBorder / USizeBox / UButton) reuse
// this by keeping at most one child.
class UPanelWidget : public UWidget
{
public:
    void AddChildWidget(std::shared_ptr<UWidget> child)
    {
        if (!child)
            return;
        m_Children.push_back(std::move(child));
        InvalidateWidget();
    }

    void RemoveChildAt(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_Children.size()))
            return;
        m_Children.erase(m_Children.begin() + index);
        InvalidateWidget();
    }

    void ClearChildrenWidgets()
    {
        m_Children.clear();
        InvalidateWidget();
    }

    int GetChildrenCount() const { return static_cast<int>(m_Children.size()); }
    std::shared_ptr<UWidget> GetChildWidgetAt(int index) const
    {
        if (index < 0 || index >= static_cast<int>(m_Children.size()))
            return nullptr;
        return m_Children[static_cast<size_t>(index)];
    }

protected:
    std::vector<std::shared_ptr<UWidget>> m_Children;
};
}  // namespace ZUMG
