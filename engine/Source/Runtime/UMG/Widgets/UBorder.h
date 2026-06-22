#pragma once

#include "ZSlate/Widgets/SBorder.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

namespace ZUMG
{
// UE UMG UBorder analogue -> ZSlate::SBorder. A filled background that wraps one
// content child.
class UBorder : public UPanelWidget
{
public:
    UIColor BackgroundColor {0.12f, 0.12f, 0.12f, 1.0f};
    bool DrawBackground {true};
    ZSlate::FMargin Padding;
    ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Fill};
    ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Fill};

    void SetContent(std::shared_ptr<UWidget> content)
    {
        ClearChildrenWidgets();
        if (content)
            AddChildWidget(std::move(content));
    }
    void SetBackgroundColor(const UIColor& color)
    {
        BackgroundColor = color;
        if (auto* w = GetSlateAs<ZSlate::SBorder>())
            w->BackgroundColor = color;
    }

    const char* GetWidgetClassName() const override { return "Border"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetColor("bg_color", BackgroundColor);
        bag.SetBool("draw_bg", DrawBackground);
        bag.SetFloat("pad_l", Padding.Left);
        bag.SetFloat("pad_t", Padding.Top);
        bag.SetFloat("pad_r", Padding.Right);
        bag.SetFloat("pad_b", Padding.Bottom);
        bag.SetInt("halign", static_cast<int>(HAlign));
        bag.SetInt("valign", static_cast<int>(VAlign));
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        BackgroundColor = bag.GetColor("bg_color", BackgroundColor);
        DrawBackground = bag.GetBool("draw_bg", DrawBackground);
        Padding = ZSlate::FMargin(bag.GetFloat("pad_l", Padding.Left), bag.GetFloat("pad_t", Padding.Top),
                                  bag.GetFloat("pad_r", Padding.Right), bag.GetFloat("pad_b", Padding.Bottom));
        HAlign = static_cast<ZSlate::EHorizontalAlignment>(bag.GetInt("halign", static_cast<int>(HAlign)));
        VAlign = static_cast<ZSlate::EVerticalAlignment>(bag.GetInt("valign", static_cast<int>(VAlign)));
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto b = std::make_shared<ZSlate::SBorder>();
        b->BackgroundColor = BackgroundColor;
        b->DrawBackground = DrawBackground;
        b->Padding = Padding;
        b->HAlign = HAlign;
        b->VAlign = VAlign;
        b->Visibility = m_Visibility;
        if (!m_Children.empty() && m_Children[0])
            b->SetContent(m_Children[0]->TakeWidget());
        return b;
    }
};
}  // namespace ZUMG
