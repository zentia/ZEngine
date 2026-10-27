#pragma once

#include "ZSlate/Widgets/SWidget.h"
#include "Runtime/UMG/Core/UMGProperties.h"
#include "Runtime/UMG/Animation/UMGAnimation.h"
#include "Runtime/UMG/Animation/UWidgetAnimation.h"
#include "Runtime/UMG/Layout/UMGAnchor.h"

#include <memory>
#include <string>
#include <vector>

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
    // Slate paint. Advances any animations this widget owns (see AddAnimation),
    // then calls NativeTick. delta_time is the frame delta in seconds.
    // Panels should override NativeTick to recurse into children (see UMGTree.h
    // for the shared recursion that skips Collapsed subtrees).
    void Tick(float delta_time)
    {
        for (UWidgetAnimation& anim : m_Animations)
        {
            anim.Tick(delta_time, m_RenderTransform);
        }
        NativeTick(delta_time);
    }

    // Subclass hook, called after this widget's animations have advanced. Default
    // no-op; override for animated / live HUD widgets (progress bars, timers).
    virtual void NativeTick(float delta_time) { (void)delta_time; }

    // --- Render transform / opacity (P1) -------------------------------------
    // The animation target this widget exposes to UWidgetAnimation. Opacity and
    // translation/scale/rotation are the UE UWidget::RenderOpacity / RenderTransform
    // analogues. Values start at their neutral defaults (opaque, no offset); a
    // UWidgetAnimation only overwrites the slots its tracks name, so a fade never
    // accidentally zeroes the scale. Applying these to the actual paint (opacity
    // multiplied into the tint / transform folded into geometry) is the ZSlate
    // paint-chain wiring on the real-machine list, not here.
    anim_core::RenderTransform& GetRenderTransform() { return m_RenderTransform; }
    const anim_core::RenderTransform& GetRenderTransform() const { return m_RenderTransform; }

    // Own an animation that drives this widget's render transform each frame.
    // Returns a reference into this widget's storage (stable for the widget's
    // lifetime); callers configure tracks via AddTrack then Play. Playing is
    // driven automatically by Tick().
    UWidgetAnimation& AddAnimation()
    {
        m_Animations.emplace_back();
        return m_Animations.back();
    }
    void ClearAnimations() { m_Animations.clear(); }
    int GetAnimationCount() const { return static_cast<int>(m_Animations.size()); }

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

    // --- Anchor / adaptive layout (P1) ---------------------------------------
    // A minimal Unity-RectTransform-style anchor model (see Layout/UMGAnchor.h for
    // the pure conversion core). anchor_min/anchor_max are normalized [0,1] points
    // inside the parent rect; min==max is a point anchor (widget keeps its own size
    // and follows the anchor), min!=max is a stretch anchor (widget fills that span
    // along the axis). pivot is this widget's own reference point [0,1]. offset is a
    // pixel offset added after the anchor pixel position. The default (anchor 0,0 /
    // pivot 0,0 / offset 0) means "top-left, no offset" — the ZSlate flow-layout
    // behaviour widgets already have, so existing widgets are unaffected.
    const layout_core::Vec2& GetAnchorMin() const { return m_AnchorMin; }
    const layout_core::Vec2& GetAnchorMax() const { return m_AnchorMax; }
    const layout_core::Vec2& GetPivot() const { return m_Pivot; }
    const layout_core::Vec2& GetOffset() const { return m_Offset; }
    void SetAnchorMin(layout_core::Vec2 a) { m_AnchorMin = layout_core::Clamp01(a); }
    void SetAnchorMax(layout_core::Vec2 a) { m_AnchorMax = layout_core::Clamp01(a); }
    void SetPivot(layout_core::Vec2 p) { m_Pivot = layout_core::Clamp01(p); }
    void SetOffset(layout_core::Vec2 o) { m_Offset = o; }

    // Resolve this widget's absolute top-left pixel position inside a parent rect
    // (parent_pos..parent_pos+parent_size) given this widget's own pixel size. Pure
    // forwarding to layout_core::ResolveTopLeft so the arithmetic is testable
    // headlessly. Applying the result to ZSlate's FGeometry (MakeChild) is the
    // render-bound wiring on the real-machine list, not here.
    layout_core::Vec2 ResolveAbsoluteTopLeft(layout_core::Vec2 parent_pos,
                                             layout_core::Vec2 parent_size,
                                             layout_core::Vec2 size) const
    {
        return layout_core::ResolveTopLeft(parent_pos, parent_size, m_AnchorMin, m_Pivot, m_Offset, size);
    }

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
        bag.SetFloat("anchor_min_x", m_AnchorMin.x);
        bag.SetFloat("anchor_min_y", m_AnchorMin.y);
        bag.SetFloat("anchor_max_x", m_AnchorMax.x);
        bag.SetFloat("anchor_max_y", m_AnchorMax.y);
        bag.SetFloat("pivot_x", m_Pivot.x);
        bag.SetFloat("pivot_y", m_Pivot.y);
        bag.SetFloat("offset_x", m_Offset.x);
        bag.SetFloat("offset_y", m_Offset.y);
    }
    virtual void DeserializeProperties(const UMGPropertyBag& bag)
    {
        m_Visibility = static_cast<ZSlate::EVisibility>(bag.GetInt("visibility", static_cast<int>(m_Visibility)));
        m_AnchorMin = layout_core::Clamp01(layout_core::Vec2{bag.GetFloat("anchor_min_x", m_AnchorMin.x), bag.GetFloat("anchor_min_y", m_AnchorMin.y)});
        m_AnchorMax = layout_core::Clamp01(layout_core::Vec2{bag.GetFloat("anchor_max_x", m_AnchorMax.x), bag.GetFloat("anchor_max_y", m_AnchorMax.y)});
        m_Pivot = layout_core::Clamp01(layout_core::Vec2{bag.GetFloat("pivot_x", m_Pivot.x), bag.GetFloat("pivot_y", m_Pivot.y)});
        m_Offset = layout_core::Vec2{bag.GetFloat("offset_x", m_Offset.x), bag.GetFloat("offset_y", m_Offset.y)};
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
    anim_core::RenderTransform m_RenderTransform;
    std::vector<UWidgetAnimation> m_Animations;
    layout_core::Vec2 m_AnchorMin {0.0f, 0.0f};
    layout_core::Vec2 m_AnchorMax {0.0f, 0.0f};
    layout_core::Vec2 m_Pivot {0.0f, 0.0f};
    layout_core::Vec2 m_Offset {0.0f, 0.0f};
    std::shared_ptr<ZSlate::SWidget> m_Widget;
};
}  // namespace ZUMG
