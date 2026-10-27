#include "Runtime/UMG/Core/UUserWidget.h"

#include "Runtime/UMG/Asset/UMGWidgetSerializer.h"
#include "Runtime/UMG/Asset/UWidgetAsset.h"
#include "Runtime/UMG/Core/UMGViewport.h"

namespace ZUMG
{
UUserWidget::~UUserWidget() = default;

bool UUserWidget::LoadFromAsset(const UWidgetAsset& asset)
{
    std::shared_ptr<UWidget> root = BuildWidgetTree(asset);
    if (!root)
        return false;
    SetPrebuiltRoot(std::move(root));
    return true;
}

void UUserWidget::EnsureBuilt()
{
    if (m_Built)
        return;
    m_Built = true;
    Build();
}

std::shared_ptr<ZSlate::SWidget> UUserWidget::TakeWidget()
{
    EnsureBuilt();
    return m_Root ? m_Root->TakeWidget() : nullptr;
}

void UUserWidget::AddToViewport(int zorder)
{
    EnsureBuilt();
    if (m_InViewport)
        return;
    m_InViewport = true;
    UMGViewport::Get().AddWidget(shared_from_this(), zorder);
}

void UUserWidget::RemoveFromViewport()
{
    if (!m_InViewport)
        return;
    m_InViewport = false;
    UMGViewport::Get().RemoveWidget(shared_from_this());
}

void UUserWidget::TickTree(float delta_time)
{
    if (!m_InViewport)
        return;  // only tick widgets that are actually on screen
    EnsureBuilt();
    ZUMG::TickWidgetTree(m_Root, delta_time, ZSlate::EVisibility::Collapsed);
}

int UUserWidget::CollectFocusableWidgets(std::vector<std::shared_ptr<UWidget>>& out) const
{
    out.clear();
    if (!m_InViewport)
        return 0;
    ZUMG::CollectFocusable(m_Root, out, ZSlate::EVisibility::Collapsed);
    // Stable-sort by TabOrder so explicit tab slots win over tree order, while
    // equal orders keep their depth-first sequence. (stable_sort, not sort.)
    std::stable_sort(out.begin(), out.end(), [](const std::shared_ptr<UWidget>& a, const std::shared_ptr<UWidget>& b) {
        return a->GetTabOrder() < b->GetTabOrder();
    });
    return static_cast<int>(out.size());
}

bool UUserWidget::AdvanceFocus(int& index_hint, bool reverse)
{
    std::vector<std::shared_ptr<UWidget>> focusable;
    const int count = CollectFocusableWidgets(focusable);
    const int next = ZUMG::NextFocusIndex(index_hint, count, reverse);
    if (next < 0 || next >= count)
    {
        index_hint = -1;
        return false;  // nothing focusable this frame
    }
    index_hint = next;
    // NOTE: actually setting ZSlate keyboard focus (focusable[next]->TakeWidget()
    // -> SlateInputRouter::SetKeyboardFocusWidget) is the render/input-bound half
    // and is wired by UISystem, which owns the router. This method computes the
    // target index purely so that half stays testable headlessly.
    return true;
}
}  // namespace ZUMG
