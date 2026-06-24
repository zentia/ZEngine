#include "Runtime/UMG/Demo/UMGDemoWidget.h"

#include "Runtime/UMG/Widgets/UBorder.h"
#include "Runtime/UMG/Widgets/UButton.h"
#include "Runtime/UMG/Widgets/UCheckBox.h"
#include "Runtime/UMG/Widgets/UEditableText.h"
#include "Runtime/UMG/Widgets/USizeBox.h"
#include "Runtime/UMG/Widgets/USlider.h"
#include "Runtime/UMG/Widgets/UTextBlock.h"
#include "Runtime/UMG/Widgets/UBoxPanel.h"

#include <cstdio>
#include <memory>
#include <string>

namespace ZUMG
{
namespace
{
std::string FormatCount(int n)
{
    return "Clicked: " + std::to_string(n);
}

std::string FormatSlider(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Slider: %.2f", static_cast<double>(v));
    return buf;
}
}  // namespace

void UMGDemoWidget::Build()
{
    auto column = std::make_shared<UVerticalBox>();

    // Title.
    auto title = std::make_shared<UTextBlock>();
    title->Text = "ZUMG Demo (UMG over ZSlate)";
    title->FontSize = 20.0f;
    title->Color = ZSlate::UIColor(0.85f, 0.90f, 1.0f, 1.0f);
    column->AddSlot(title).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f));

    // Counter label + button.
    m_CounterLabel = std::make_shared<UTextBlock>();
    m_CounterLabel->Text = FormatCount(m_ClickCount);
    m_CounterLabel->FontSize = 15.0f;
    column->AddSlot(m_CounterLabel).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    auto button = std::make_shared<UButton>();
    auto button_label = std::make_shared<UTextBlock>();
    button_label->Text = "Increment";
    button_label->FontSize = 15.0f;
    button->SetContent(button_label);
    {
        std::weak_ptr<UTextBlock> weak_label = m_CounterLabel;
        UMGDemoWidget* self = this;
        button->OnClicked = [self, weak_label]() {
            self->m_ClickCount += 1;
            if (auto label = weak_label.lock())
                label->SetText(FormatCount(self->m_ClickCount));
        };
    }
    column->AddSlot(button).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f));

    // Editable text.
    auto edit = std::make_shared<UEditableText>();
    edit->HintText = "Type here...";
    edit->FontSize = 15.0f;
    edit->MinWidth = 220.0f;
    column->AddSlot(edit).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f));

    // Check box row.
    auto check_row = std::make_shared<UHorizontalBox>();
    auto check = std::make_shared<UCheckBox>();
    check->Checked = true;
    auto check_text = std::make_shared<UTextBlock>();
    check_text->Text = "Enabled";
    check_text->FontSize = 15.0f;
    check_row->AddSlot(check).AutoSize().SetVAlign(ZSlate::EVerticalAlignment::Center);
    check_row->AddSlot(check_text)
        .AutoSize()
        .SetVAlign(ZSlate::EVerticalAlignment::Center)
        .SetPadding(ZSlate::FMargin(8.0f, 0.0f, 0.0f, 0.0f));
    column->AddSlot(check_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f));

    // Slider + live value label.
    m_SliderLabel = std::make_shared<UTextBlock>();
    m_SliderLabel->Text = FormatSlider(0.35f);
    m_SliderLabel->FontSize = 15.0f;
    column->AddSlot(m_SliderLabel).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f));

    auto slider = std::make_shared<USlider>();
    slider->Value = 0.35f;
    slider->MinDesiredWidth = 220.0f;
    {
        std::weak_ptr<UTextBlock> weak_value = m_SliderLabel;
        slider->OnValueChanged = [weak_value](float v) {
            if (auto label = weak_value.lock())
                label->SetText(FormatSlider(v));
        };
    }
    column->AddSlot(slider).AutoSize();

    // Wrap the column in a padded background border.
    auto border = std::make_shared<UBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.11f, 0.13f, 0.96f);
    border->Padding = ZSlate::FMargin(16.0f);
    border->HAlign = ZSlate::EHorizontalAlignment::Left;
    border->VAlign = ZSlate::EVerticalAlignment::Top;
    border->SetContent(column);

    // The viewport stretches its slots full-screen; a top-left-aligned SizeBox
    // keeps the demo panel at its content size in the corner instead of filling
    // the whole display.
    auto anchor = std::make_shared<USizeBox>();
    anchor->HAlign = ZSlate::EHorizontalAlignment::Left;
    anchor->VAlign = ZSlate::EVerticalAlignment::Top;
    anchor->SetContent(border);

    SetRootWidget(anchor);
}
}  // namespace ZUMG
