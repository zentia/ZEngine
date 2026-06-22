#pragma once

#include "ZSlate/Widgets/SBox.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

namespace ZUMG
{
// UE UMG USizeBox analogue -> ZSlate::SBox. Pins / clamps the size of its single
// content child. A negative override means "use the child's desired size".
class USizeBox : public UPanelWidget
{
public:
    float WidthOverride {-1.0f};
    float HeightOverride {-1.0f};
    float MinDesiredWidth {-1.0f};
    float MinDesiredHeight {-1.0f};
    float MaxDesiredWidth {-1.0f};
    float MaxDesiredHeight {-1.0f};
    ZSlate::EHorizontalAlignment HAlign {ZSlate::EHorizontalAlignment::Fill};
    ZSlate::EVerticalAlignment VAlign {ZSlate::EVerticalAlignment::Fill};

    void SetContent(std::shared_ptr<UWidget> content)
    {
        ClearChildrenWidgets();
        if (content)
            AddChildWidget(std::move(content));
    }

    const char* GetWidgetClassName() const override { return "SizeBox"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetFloat("width_override", WidthOverride);
        bag.SetFloat("height_override", HeightOverride);
        bag.SetFloat("min_width", MinDesiredWidth);
        bag.SetFloat("min_height", MinDesiredHeight);
        bag.SetFloat("max_width", MaxDesiredWidth);
        bag.SetFloat("max_height", MaxDesiredHeight);
        bag.SetInt("halign", static_cast<int>(HAlign));
        bag.SetInt("valign", static_cast<int>(VAlign));
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        WidthOverride = bag.GetFloat("width_override", WidthOverride);
        HeightOverride = bag.GetFloat("height_override", HeightOverride);
        MinDesiredWidth = bag.GetFloat("min_width", MinDesiredWidth);
        MinDesiredHeight = bag.GetFloat("min_height", MinDesiredHeight);
        MaxDesiredWidth = bag.GetFloat("max_width", MaxDesiredWidth);
        MaxDesiredHeight = bag.GetFloat("max_height", MaxDesiredHeight);
        HAlign = static_cast<ZSlate::EHorizontalAlignment>(bag.GetInt("halign", static_cast<int>(HAlign)));
        VAlign = static_cast<ZSlate::EVerticalAlignment>(bag.GetInt("valign", static_cast<int>(VAlign)));
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto box = std::make_shared<ZSlate::SBox>();
        box->WidthOverride = WidthOverride;
        box->HeightOverride = HeightOverride;
        box->MinDesiredWidth = MinDesiredWidth;
        box->MinDesiredHeight = MinDesiredHeight;
        box->MaxDesiredWidth = MaxDesiredWidth;
        box->MaxDesiredHeight = MaxDesiredHeight;
        box->HAlign = HAlign;
        box->VAlign = VAlign;
        box->Visibility = m_Visibility;
        if (!m_Children.empty() && m_Children[0])
            box->SetContent(m_Children[0]->TakeWidget());
        return box;
    }
};
}  // namespace ZUMG
