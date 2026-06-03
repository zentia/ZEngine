#pragma once

#include "Runtime/Slate/Widgets/SWidget.h"
#include "Runtime/UMG/Core/UMGProperties.h"

#include <memory>
#include <string>

// ZUMG: a UE-style UMG (Unreal Motion Graphics) widget object layer built on top
// of the retained-mode ZSlate framework (engine/Source/Runtime/Slate). Each
// UWidget is a high-level wrapper that constructs and owns an underlying
// ZSlate::SWidget; user code (and, in later phases, serialized assets and the
// editor designer) composes a tree of UWidgets, then realises it into a Slate
// tree for layout / paint / input.
//
// P1 scope: plain std::shared_ptr-based object model (no reflection / no
// serialization). Reflection (REGISTER_CLASS + Transfer) and an Object base land
// in P2; the public field/accessor surface here is intentionally
// serialization-friendly so that migration is mechanical.
namespace ZUMG
{
class UWidget : public std::enable_shared_from_this<UWidget>
{
public:
    virtual ~UWidget() = default;

    // Designer / hierarchy name (UE UWidget::GetName analogue).
    std::string Name;

    // Lazily build (once) and return the underlying ZSlate widget. Structural
    // edits should call InvalidateWidget() first so the next call rebuilds.
    const std::shared_ptr<ZSlate::SWidget>& TakeWidget()
    {
        if (!m_Widget)
        {
            m_Widget = RebuildWidget();
        }
        return m_Widget;
    }

    bool HasWidget() const { return m_Widget != nullptr; }

    // Drop the cached Slate widget so the next TakeWidget() rebuilds the subtree.
    // Children override-friendly: panels call this when their child set changes.
    virtual void InvalidateWidget() { m_Widget.reset(); }

    void SetVisibility(ZSlate::EVisibility visibility)
    {
        m_Visibility = visibility;
        if (m_Widget)
            m_Widget->Visibility = visibility;
    }
    ZSlate::EVisibility GetVisibility() const { return m_Visibility; }

    // --- Reflection / serialization surface (P2) -----------------------------
    // Stable class identity used by the widget factory and the on-disk node
    // (UWidgetAsset). Must match UMGWidgetFactory::Create().
    virtual const char* GetWidgetClassName() const = 0;

    // Write / read this widget's authoring state to / from a typed property bag.
    // Subclasses override to add their own fields and MUST chain to the base so
    // common properties (visibility) round-trip. The bag is the single contract
    // shared by the asset serializer and the P3 designer's property panel.
    virtual void SerializeProperties(UMGPropertyBag& bag) const
    {
        bag.SetInt("visibility", static_cast<int>(m_Visibility));
    }
    virtual void DeserializeProperties(const UMGPropertyBag& bag)
    {
        m_Visibility = static_cast<ZSlate::EVisibility>(bag.GetInt("visibility", static_cast<int>(m_Visibility)));
    }

protected:
    // Concrete widgets construct their ZSlate widget here and apply their current
    // property values (including m_Visibility). Panels recurse via child
    // TakeWidget().
    virtual std::shared_ptr<ZSlate::SWidget> RebuildWidget() = 0;

    // Typed access to the built Slate widget (null before first TakeWidget()).
    template<typename T>
    T* GetSlateAs()
    {
        return static_cast<T*>(m_Widget.get());
    }

    ZSlate::EVisibility m_Visibility {ZSlate::EVisibility::Visible};
    std::shared_ptr<ZSlate::SWidget> m_Widget;
};
}  // namespace ZUMG
