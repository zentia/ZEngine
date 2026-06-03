#pragma once

#include "Runtime/Slate/Widgets/SCheckBox.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <functional>

namespace ZUMG
{
// UE UMG UCheckBox analogue -> ZSlate::SCheckBox.
class UCheckBox : public UWidget
{
public:
    bool Checked {false};
    float BoxSize {18.0f};
    std::function<void(bool)> OnCheckStateChanged;

    bool IsChecked() const
    {
        if (m_Widget)
            return static_cast<const ZSlate::SCheckBox*>(m_Widget.get())->Checked;
        return Checked;
    }
    void SetChecked(bool checked)
    {
        Checked = checked;
        if (auto* w = GetSlateAs<ZSlate::SCheckBox>())
            w->Checked = checked;
    }

    const char* GetWidgetClassName() const override { return "CheckBox"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetBool("checked", IsChecked());
        bag.SetFloat("box_size", BoxSize);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Checked = bag.GetBool("checked", Checked);
        BoxSize = bag.GetFloat("box_size", BoxSize);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto c = std::make_shared<ZSlate::SCheckBox>();
        c->Checked = Checked;
        c->BoxSize = BoxSize;
        c->OnCheckStateChanged = OnCheckStateChanged;
        c->Visibility = m_Visibility;
        return c;
    }
};
}  // namespace ZUMG
