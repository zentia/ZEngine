#pragma once

#include "Runtime/Slate/Widgets/SEditableTextBox.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <functional>
#include <string>

namespace ZUMG
{
// UE UMG UEditableText analogue -> ZSlate::SEditableTextBox. Single-line input.
class UEditableText : public UWidget
{
public:
    std::string Text;
    std::string HintText;
    float FontSize {16.0f};
    float MinWidth {140.0f};
    std::function<void(const std::string&)> OnTextChanged;
    std::function<void(const std::string&)> OnTextCommitted;

    std::string GetText() const
    {
        if (m_Widget)
            return static_cast<const ZSlate::SEditableTextBox*>(m_Widget.get())->Text;
        return Text;
    }
    void SetText(const std::string& text)
    {
        Text = text;
        if (auto* w = GetSlateAs<ZSlate::SEditableTextBox>())
        {
            // Don't clobber while the user is actively typing.
            if (!w->IsFocused())
                w->Text = text;
        }
    }

    const char* GetWidgetClassName() const override { return "EditableText"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        bag.SetString("text", GetText());
        bag.SetString("hint", HintText);
        bag.SetFloat("font_size", FontSize);
        bag.SetFloat("min_width", MinWidth);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Text = bag.GetString("text", Text);
        HintText = bag.GetString("hint", HintText);
        FontSize = bag.GetFloat("font_size", FontSize);
        MinWidth = bag.GetFloat("min_width", MinWidth);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto e = std::make_shared<ZSlate::SEditableTextBox>();
        e->Text = Text;
        e->HintText = HintText;
        e->FontSize = FontSize;
        e->MinWidth = MinWidth;
        e->OnTextChanged = OnTextChanged;
        e->OnTextCommitted = OnTextCommitted;
        e->Visibility = m_Visibility;
        return e;
    }
};
}  // namespace ZUMG
