#pragma once

#include "ZSlate/Widgets/SSlider.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <functional>

namespace ZUMG
{
// UE UMG USlider analogue -> ZSlate::SSlider. Value normalised to [0,1].
class USlider : public UWidget
{
public:
    float Value {0.0f};
    float MinDesiredWidth {120.0f};
    std::function<void(float)> OnValueChanged;

    float GetValue() const
    {
        if (m_Widget)
            return static_cast<const ZSlate::SSlider*>(m_Widget.get())->Value;
        return Value;
    }
    void SetValue(float value)
    {
        Value = value;
        if (auto* w = GetSlateAs<ZSlate::SSlider>())
            w->Value = value;
    }

    const char* GetWidgetClassName() const override { return "Slider"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetFloat("value", GetValue());
        bag.SetFloat("min_width", MinDesiredWidth);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Value = bag.GetFloat("value", Value);
        MinDesiredWidth = bag.GetFloat("min_width", MinDesiredWidth);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto s = std::make_shared<ZSlate::SSlider>();
        s->Value = Value;
        s->MinDesiredWidth = MinDesiredWidth;
        s->OnValueChanged = OnValueChanged;
        s->Visibility = m_Visibility;
        return s;
    }
};
}  // namespace ZUMG
