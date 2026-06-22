#pragma once

#include "ZSlate/Widgets/SScrollBox.h"
#include "Runtime/UMG/Core/UPanelWidget.h"

namespace ZUMG
{
// UE UMG UScrollBox analogue -> ZSlate::SScrollBox. A vertically scrolling
// container; children stack top-to-bottom at their desired heights.
class UScrollBox : public UPanelWidget
{
public:
    float ScrollSpeed {40.0f};

    const char* GetWidgetClassName() const override { return "ScrollBox"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetFloat("scroll_speed", ScrollSpeed);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        ScrollSpeed = bag.GetFloat("scroll_speed", ScrollSpeed);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto scroll = std::make_shared<ZSlate::SScrollBox>();
        scroll->ScrollSpeed = ScrollSpeed;
        scroll->Visibility = m_Visibility;
        for (const auto& child : m_Children)
        {
            if (child)
                scroll->AddChild(child->TakeWidget());
        }
        return scroll;
    }
};
}  // namespace ZUMG
