#pragma once

#include "ZSlate/Widgets/Panels/SBorder.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

namespace ZUMG
{
// UE UMG USafeZone analogue. A single-content panel that reserves the viewport's
// system safe area (notch / rounded corners / TV overscan) so HUD elements stay
// inside the visible region.
//
// Implementation reuses ZSlate::SBorder with the background disabled and the
// safe-area insets as its Padding — SBorder already insets its content by
// Padding and aligns it, so this gives the correct "push the child inside the
// safe area" behaviour with no new ZSlate widget. The insets are authored here
// (pixels); reading the real per-platform safe area from the window/backend and
// feeding it into these fields each frame is the render-bound wiring on the
// real-machine list — until that lands, the defaults are 0 (behaves like a
// transparent passthrough that still honours any authored padding).
class USafeZone : public UPanelWidget
{
public:
    float PaddingLeft {0.0f};
    float PaddingTop {0.0f};
    float PaddingRight {0.0f};
    float PaddingBottom {0.0f};

    // Convenience: set a symmetric inset (the common "keep everything Npx from
    // every edge" case).
    void SetUniformPadding(float px)
    {
        PaddingLeft = PaddingTop = PaddingRight = PaddingBottom = px;
        InvalidateWidget();
    }

    const char* GetWidgetClassName() const override { return "SafeZone"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetFloat("pad_l", PaddingLeft);
        bag.SetFloat("pad_t", PaddingTop);
        bag.SetFloat("pad_r", PaddingRight);
        bag.SetFloat("pad_b", PaddingBottom);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        PaddingLeft = bag.GetFloat("pad_l", PaddingLeft);
        PaddingTop = bag.GetFloat("pad_t", PaddingTop);
        PaddingRight = bag.GetFloat("pad_r", PaddingRight);
        PaddingBottom = bag.GetFloat("pad_b", PaddingBottom);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto border = std::make_shared<ZSlate::SBorder>();
        border->DrawBackground = false;  // pure inset container, no visible frame
        border->Padding = ZSlate::FMargin(PaddingLeft, PaddingTop, PaddingRight, PaddingBottom);
        border->HAlign = ZSlate::EHorizontalAlignment::Fill;
        border->VAlign = ZSlate::EVerticalAlignment::Fill;
        border->Visibility = m_Visibility;
        if (!m_Children.empty() && m_Children[0])
            border->SetContent(m_Children[0]->TakeWidget());
        return border;
    }
};
}  // namespace ZUMG
