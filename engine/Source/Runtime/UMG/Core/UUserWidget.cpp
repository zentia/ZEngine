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
}  // namespace ZUMG
