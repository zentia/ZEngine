#pragma once

#include "ZSlate/Widgets/SProgressBar.h"
#include "Runtime/UMG/Core/UWidget.h"

namespace ZUMG
{
// UE UMG UProgressBar analogue -> ZSlate::SProgressBar. Fill fraction 0..1.
class UProgressBar : public UWidget
{
public:
    float Value {0.0f};  // 0..1
    bool IsHorizontal {true};
    ZSlate::UIColor BackgroundColor {0.12f, 0.12f, 0.14f, 1.0f};
    ZSlate::UIColor FillColor {0.30f, 0.55f, 0.95f, 1.0f};
    ZSlate::UIColor BorderColor {0.35f, 0.38f, 0.45f, 1.0f};
    float BorderThickness {1.0f};

    float GetValue() const
    {
        if (m_Widget)
            return static_cast<const ZSlate::SProgressBar*>(m_Widget.get())->Percent;
        return Value;
    }
    void SetValue(float value)
    {
        Value = value;
        if (auto* w = GetSlateAs<ZSlate::SProgressBar>())
            w->Percent = value;
    }

    const char* GetWidgetClassName() const override { return "ProgressBar"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetFloat("value", GetValue());
        bag.SetBool("horizontal", IsHorizontal);
        bag.SetColor("bg_color", BackgroundColor);
        bag.SetColor("fill_color", FillColor);
        bag.SetColor("border_color", BorderColor);
        bag.SetFloat("border_thickness", BorderThickness);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Value = bag.GetFloat("value", Value);
        IsHorizontal = bag.GetBool("horizontal", IsHorizontal);
        BackgroundColor = bag.GetColor("bg_color", BackgroundColor);
        FillColor = bag.GetColor("fill_color", FillColor);
        BorderColor = bag.GetColor("border_color", BorderColor);
        BorderThickness = bag.GetFloat("border_thickness", BorderThickness);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto bar = std::make_shared<ZSlate::SProgressBar>();
        bar->Orientation = IsHorizontal ? ZSlate::EOrientation::Horizontal : ZSlate::EOrientation::Vertical;
        bar->Percent = Value;
        bar->BackgroundColor = ZSlate::UIColor(BackgroundColor.x, BackgroundColor.y, BackgroundColor.z, BackgroundColor.w);
        bar->FillColor = ZSlate::UIColor(FillColor.x, FillColor.y, FillColor.z, FillColor.w);
        bar->BorderColor = ZSlate::UIColor(BorderColor.x, BorderColor.y, BorderColor.z, BorderColor.w);
        bar->BorderThickness = BorderThickness;
        bar->Visibility = m_Visibility;
        return bar;
    }
};
}  // namespace ZUMG
