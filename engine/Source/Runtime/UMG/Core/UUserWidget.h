#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <memory>

class UWidgetAsset;

namespace ZUMG
{
// UE UMG UUserWidget analogue: a composed widget whose tree is authored either in
// C++ (override Build() and SetRootWidget()) or, from P2 onward, loaded from a
// UWidgetTree asset. AddToViewport() realises the tree into Slate and stacks it
// into the process UMGViewport.
class UUserWidget : public std::enable_shared_from_this<UUserWidget>
{
public:
    virtual ~UUserWidget();

    // Authoring hook. Subclasses construct their UWidget tree and call
    // SetRootWidget(). Runs once, lazily, on first TakeWidget()/AddToViewport().
    virtual void Build() {}

    void SetRootWidget(std::shared_ptr<UWidget> root) { m_Root = std::move(root); }
    std::shared_ptr<UWidget> GetRootWidget() const { return m_Root; }

    // P2: install an already-built tree (e.g. from a UWidgetAsset) and mark this
    // user widget as built so AddToViewport() won't re-run Build().
    void SetPrebuiltRoot(std::shared_ptr<UWidget> root)
    {
        m_Root = std::move(root);
        m_Built = true;
    }

    // Build the tree from a serialized widget asset. Returns false if the asset
    // is empty / malformed.
    bool LoadFromAsset(const UWidgetAsset& asset);

    // Build (once) and return the realised ZSlate root (null if no tree built).
    std::shared_ptr<ZSlate::SWidget> TakeWidget();

    void AddToViewport(int zorder = 0);
    void RemoveFromViewport();
    bool IsInViewport() const { return m_InViewport; }

protected:
    void EnsureBuilt();

    std::shared_ptr<UWidget> m_Root;
    bool m_Built {false};
    bool m_InViewport {false};
};
}  // namespace ZUMG
