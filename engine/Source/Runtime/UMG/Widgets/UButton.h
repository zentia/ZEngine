#pragma once

#include "ZSlate/Widgets/Input/SButton.h"
#include "Runtime/UI/Core/UITypes.h"
#include "Runtime/UMG/Core/UPanelWidget.h"
#include "Runtime/UMG/Core/UMGDelegates.h"

#include <functional>
#include <string>

namespace ZUMG
{
// UE UMG UButton analogue -> ZSlate::SButton. A clickable panel wrapping one
// content child (usually a UTextBlock).
class UButton : public UPanelWidget
{
public:
    UButton() { m_Focusable = true; }  // interactive: focusable by default

    ::ZSlate::UIColor NormalColor {0.20f, 0.20f, 0.22f, 1.0f};
    ::ZSlate::UIColor HoverColor {0.28f, 0.28f, 0.32f, 1.0f};
    ::ZSlate::UIColor PressedColor {0.15f, 0.15f, 0.17f, 1.0f};
    ZSlate::FMargin Padding {8.0f, 4.0f};
    ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Center};
    ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Center};

    std::function<void()> OnClicked;

    // P0 data-binding: when set, the button fires this named event through the
    // process-wide UMGEventRegistry on click (in ADDITION to the code-bound
    // OnClicked, which stays for C++-authored widgets). This is what lets a
    // Designer-authored asset wire a click to game code without a C++ subclass:
    // the asset stores the event name, the game binds the handler by name at
    // startup. Empty string = no registry event.
    std::string OnClickEvent;

    void SetContent(std::shared_ptr<UWidget> content)
    {
        ClearChildrenWidgets();
        if (content)
            AddChildWidget(std::move(content));
    }

    const char* GetWidgetClassName() const override { return "Button"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        // P0: the event NAME is serialised (the handler itself stays code-bound —
        // std::function cannot be persisted). Loaded assets get an empty OnClicked
        // but a valid OnClickEvent; the game binds the handler by name at startup.
        bag.SetString("on_click_event", OnClickEvent);
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
        OnClickEvent = bag.GetString("on_click_event", OnClickEvent);
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
        // Merge the two click paths: the code-bound OnClicked (C++-authored
        // widgets) AND the named-registry event (Designer-authored assets). Both
        // fire on a single click. We snapshot the event name now because the
        // SButton outlives this UButton's property reads and the lambda must not
        // capture `this` (the UButton may be destroyed before the SButton).
        const std::string click_event = OnClickEvent;
        auto code_handler = OnClicked;
        btn->OnClicked = [click_event, code_handler]()
        {
            if (code_handler)
                code_handler();
            if (!click_event.empty())
                GetUMGEventRegistry().Invoke(click_event);
        };
        btn->Visibility = m_Visibility;
        if (!m_Children.empty() && m_Children[0])
            btn->SetContent(m_Children[0]->TakeWidget());
        return btn;
    }
};
}  // namespace ZUMG
