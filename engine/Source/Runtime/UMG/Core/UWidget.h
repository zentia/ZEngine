#pragma once

#include "ZSlate/Widgets/SWidget.h"
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

    // --- Per-frame tick (P1, wired now so P0 nav can rely on it) -------------
    // Called once per frame for every widget in an in-viewport tree, before the
    // Slate paint. Default no-op; subclasses override for animated / live HUD
    // widgets (progress bars, timers). delta_time is the frame delta in seconds.
    // Panels should override to recurse into children (see UMGTick.h for the
    // shared recursion that skips Collapsed subtrees).
    virtual void NativeTick(float delta_time) { (void)delta_time; }

    // --- Keyboard focus / navigation (P0) ------------------------------------
    // Whether this widget can take keyboard focus (UE UWidget::IsFocusable
    // analogue). Interactive widgets (button, slider, editable text) return true.
    bool IsFocusable() const { return m_Focusable; }
    void SetFocusable(bool focusable) { m_Focusable = focusable; }

    // Tab-order weight: focusable widgets are collected in depth-first tree
    // order and Tab moves through them; a smaller weight sorts earlier. Default
    // 0 = use tree order. (UE has explicit tab-order slots; this is the minimal
    // subset that makes "Tab cycles through my buttons in the order I added
    // them" work without a full navigation-rule graph.)
    int GetTabOrder() const { return m_TabOrder; }
    void SetTabOrder(int order) { m_TabOrder = order; }

    // Child access for tree traversal (Tick / focus collection). UWidget is a
    // leaf by default (0 children); UPanelWidget overrides to expose its
    // children. This keeps the traversal header-only and dependency-light
    // without a downcast or a children-view trait.
    virtual int GetChildCount() const { return 0; }
    virtual std::shared_ptr<UWidget> GetChildAt(int index) const { (void)index; return nullptr; }

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
    bool m_Focusable {false};
    int m_TabOrder {0};
    std::shared_ptr<ZSlate::SWidget> m_Widget;
};
}  // namespace ZUMG
