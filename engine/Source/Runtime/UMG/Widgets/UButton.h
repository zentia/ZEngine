#pragma once

#include "ZSlate/Widgets/Input/SButton.h"
#include "Runtime/UI/Core/UITypes.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

#include <functional>

namespace ZUMG
{
// UE UMG UButton analogue -> ZSlate::SButton. A clickable panel wrapping one
// content child (usually a UTextBlock).
class UButton : public UPanelWidget
{
public:
    ::ZSlate::UIColor NormalColor {0.20f, 0.20f, 0.22f, 1.0f};
    ::ZSlate::UIColor HoverColor {0.28f, 0.28f, 0.32f, 1.0f};
    ::ZSlate::UIColor PressedColor {0.15f, 0.15f, 0.17f, 1.0f};
    ZSlate::FMargin Padding {8.0f, 4.0f};
    ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Center};
    ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Center};

    std::function<void()> OnClicked;

    void SetContent(std::shared_ptr<UWidget> content)
    {
        ClearChildrenWidgets();
        if (content)
            AddChildWidget(std::move(content));
    }

    const char* GetWidgetClassName() const override { return "Button"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        // NOTE: OnClicked is code-bound, not serialised (P2). The designer wires
        // events in a later phase; loaded assets get a no-op handler.
        UWidget::SerializeProperties(bag);
        bag.SetColor("normal_color", NormalColor);
        bag.SetColor("hover_color", HoverColor);
        bag.SetColor("pressed_color", PressedColor);
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
        NormalColor = bag.GetColor("normal_color", NormalColor);
        HoverColor = bag.GetColor("hover_color", HoverColor);
        PressedColor = bag.GetColor("pressed_color", PressedColor);
        Padding = ZSlate::FMargin(bag.GetFloat("pad_l", Padding.Left), bag.GetFloat("pad_t", Padding.Top),
                                  bag.GetFloat("pad_r", Padding.Right), bag.GetFloat("pad_b", Padding.Bottom));
        HAlign = static_cast<ZSlate::EHorizontalAlignment>(bag.GetInt("halign", static_cast<int>(HAlign)));
        VAlign = static_cast<ZSlate::EVerticalAlignment>(bag.GetInt("valign", static_cast<int>(VAlign)));
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto btn = std::make_shared<ZSlate::SButton>();
        // Convert ::ZSlate::UIColor to ZSlate::UIColor
        btn->NormalColor = ZSlate::UIColor(NormalColor.x, NormalColor.y, NormalColor.z, NormalColor.w);
        btn->HoverColor = ZSlate::UIColor(HoverColor.x, HoverColor.y, HoverColor.z, HoverColor.w);
        btn->PressedColor = ZSlate::UIColor(PressedColor.x, PressedColor.y, PressedColor.z, PressedColor.w);
        btn->Padding = Padding;
        btn->HAlign = HAlign;
        btn->VAlign = VAlign;
        btn->OnClicked = OnClicked;
        btn->Visibility = m_Visibility;
        if (!m_Children.empty() && m_Children[0])
            btn->SetContent(m_Children[0]->TakeWidget());
        return btn;
    }
};
}  // namespace ZUMG
