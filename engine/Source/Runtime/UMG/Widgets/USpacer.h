#pragma once

#include "Runtime/Slate/Widgets/SSpacer.h"
#include "Runtime/UMG/Core/UWidget.h"

namespace ZUMG
{
// UE UMG USpacer analogue -> ZSlate::SSpacer. Fixed-size empty layout filler.
class USpacer : public UWidget
{
public:
    Vector2 Size {0.0f, 0.0f};

    const char* GetWidgetClassName() const override { return "Spacer"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetVec2("size", Size);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Size = bag.GetVec2("size", Size);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto s = std::make_shared<ZSlate::SSpacer>(Size);
        s->Visibility = m_Visibility;
        return s;
    }
};
}  // namespace ZUMG
