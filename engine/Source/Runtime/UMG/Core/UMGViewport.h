#pragma once

#include "Runtime/Slate/Widgets/SOverlay.h"

#include <memory>
#include <vector>

namespace ZUMG
{
class UUserWidget;

// Process-level UMG viewport (UE UGameViewportClient + viewport widget analogue).
// Maintains a single ZSlate::SOverlay installed as the SlateApplication root;
// UserWidgets added via AddToViewport() are z-stacked into it. The actual paint /
// input is driven by the existing runtime path
// (UISystem::PreRender -> SlateApplication::GetRootContent), so no new render
// pass or engine system is needed.
class UMGViewport
{
public:
    static UMGViewport& Get();

    void AddWidget(const std::shared_ptr<UUserWidget>& widget, int zorder);
    void RemoveWidget(const std::shared_ptr<UUserWidget>& widget);
    bool ContainsWidget(const std::shared_ptr<UUserWidget>& widget) const;

    // Drop all widgets and detach from SlateApplication (e.g. on level unload).
    void Clear();

private:
    UMGViewport();

    // Re-sort by z-order and repopulate the overlay's slots, then ensure the
    // overlay is the SlateApplication root.
    void Rebuild();

    struct Entry
    {
        std::shared_ptr<UUserWidget> Widget;
        int ZOrder {0};
    };

    std::shared_ptr<ZSlate::SOverlay> m_Overlay;
    std::vector<Entry> m_Entries;
};
}  // namespace ZUMG
