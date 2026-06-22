#include "Runtime/UMG/Core/UMGViewport.h"

#include "ZSlate/Application/SlateApplication.h"
#include "Runtime/UMG/Core/UUserWidget.h"

#include <algorithm>

namespace ZUMG
{
UMGViewport& UMGViewport::Get()
{
    static UMGViewport s_instance;
    return s_instance;
}

UMGViewport::UMGViewport()
    : m_Overlay(std::make_shared<ZSlate::SOverlay>())
{
}

void UMGViewport::AddWidget(const std::shared_ptr<UUserWidget>& widget, int zorder)
{
    if (!widget)
        return;
    for (Entry& e : m_Entries)
    {
        if (e.Widget == widget)
        {
            e.ZOrder = zorder;
            Rebuild();
            return;
        }
    }
    m_Entries.push_back({widget, zorder});
    Rebuild();
}

void UMGViewport::RemoveWidget(const std::shared_ptr<UUserWidget>& widget)
{
    const size_t before = m_Entries.size();
    m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
                                   [&](const Entry& e) { return e.Widget == widget; }),
                    m_Entries.end());
    if (m_Entries.size() != before)
        Rebuild();
}

bool UMGViewport::ContainsWidget(const std::shared_ptr<UUserWidget>& widget) const
{
    for (const Entry& e : m_Entries)
    {
        if (e.Widget == widget)
            return true;
    }
    return false;
}

void UMGViewport::Clear()
{
    m_Entries.clear();
    m_Overlay->ClearChildren();
    // Detach only if we currently own the root (avoid stomping an editor surface).
    if (ZSlate::SlateApplication::Get().GetRootContent() == m_Overlay)
        ZSlate::SlateApplication::Get().SetRootContent(nullptr);
}

void UMGViewport::Rebuild()
{
    // Stable sort by z-order so equal z-orders keep insertion order; lower z-order
    // is added (and thus painted) first, higher z-order ends up on top.
    std::stable_sort(m_Entries.begin(), m_Entries.end(),
                     [](const Entry& a, const Entry& b) { return a.ZOrder < b.ZOrder; });

    m_Overlay->ClearChildren();
    for (const Entry& e : m_Entries)
    {
        if (!e.Widget)
            continue;
        if (auto slate = e.Widget->TakeWidget())
            m_Overlay->AddSlot(slate);
    }

    if (m_Entries.empty())
    {
        if (ZSlate::SlateApplication::Get().GetRootContent() == m_Overlay)
            ZSlate::SlateApplication::Get().SetRootContent(nullptr);
    }
    else
    {
        ZSlate::SlateApplication::Get().SetRootContent(m_Overlay);
    }
}
}  // namespace ZUMG
