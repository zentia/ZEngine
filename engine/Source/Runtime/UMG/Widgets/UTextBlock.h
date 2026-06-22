#pragma once

#include "ZSlate/Widgets/Text/STextBlock.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <string>

namespace ZUMG
{
// UE UMG UTextBlock analogue -> ZSlate::STextBlock.
class UTextBlock : public UWidget
{
public:
    std::string Text;
    float FontSize {14.0f};
    UIColor Color {1.0f, 1.0f, 1.0f, 1.0f};
    TextAnchor Alignment {TextAnchor::MiddleLeft};

    void SetText(const std::string& text)
    {
        Text = text;
        if (auto* w = GetSlateAs<ZSlate::STextBlock>())
            w->Text = text;
    }
    void SetColor(const UIColor& color)
    {
        Color = color;
        if (auto* w = GetSlateAs<ZSlate::STextBlock>())
            w->SetColor(ZSlate::UIColor(color.x, color.y, color.z, color.w));
    }

    const char* GetWidgetClassName() const override { return "TextBlock"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetString("text", Text);
        bag.SetFloat("font_size", FontSize);
        bag.SetColor("color", Color);
        bag.SetInt("alignment", static_cast<int>(Alignment));
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Text = bag.GetString("text", Text);
        FontSize = bag.GetFloat("font_size", FontSize);
        Color = bag.GetColor("color", Color);
        Alignment = static_cast<TextAnchor>(bag.GetInt("alignment", static_cast<int>(Alignment)));
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto t = std::make_shared<ZSlate::STextBlock>();
        t->Text = Text;
        t->FontSize = FontSize;
        t->Color = ZSlate::UIColor(Color.x, Color.y, Color.z, Color.w);
        t->Alignment = static_cast<ZSlate::TextAnchor>(Alignment);
        t->Visibility = m_Visibility;
        return t;
    }
};
}  // namespace ZUMG
