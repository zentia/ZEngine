#pragma once
// ============================================================================
// FieldDialog — the ZSlate stand-in for the small WinForms dialogs of the C#
// build (UrlEditorDialog, TipEditorDialog, NoteEditorDialog, …).
//
// Shows a modal dialog with one labelled input per field and hands the edited
// values to `onOk`. Cancel simply closes without invoking the callback.
// ============================================================================

#include "ZSlate/Widgets/Dialogs/SDialog.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Text/SMultiLineEditableText.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Input/SButton.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ClientProj {

struct FieldSpec {
    std::string Label;
    std::string Value;
    bool        Multiline = false;
    float       MinWidth  = 380.0f;
    float       Height    = 0.0f;    // multiline only; 0 -> 120
};

inline std::shared_ptr<ZSlate::SButton> DlgButton(const std::string& text,
                                                  std::function<void()> onClicked)
{
    auto button  = std::make_shared<ZSlate::SButton>();
    auto label   = std::make_shared<ZSlate::STextBlock>();
    label->Text     = text;
    label->FontSize = 13.0f;
    label->Color    = ZSlate::Colors::White;
    button->Padding = ZSlate::FMargin(14.0f, 5.0f);
    button->SetContent(label);
    button->OnClicked = std::move(onClicked);
    return button;
}

inline void ShowFieldsDialog(const std::string& title,
                             const std::vector<FieldSpec>& fields,
                             std::function<void(const std::vector<std::string>&)> onOk)
{
    auto dialog = std::make_shared<ZSlate::SDialog>();
    dialog->SetTitle(title);

    auto body = std::make_shared<ZSlate::SVerticalBox>();

    auto singleBoxes  = std::make_shared<std::vector<std::shared_ptr<ZSlate::SEditableTextBox>>>();
    auto multiBoxes   = std::make_shared<std::vector<std::shared_ptr<ZSlate::SMultiLineEditableText>>>();
    auto fieldIsMulti = std::make_shared<std::vector<bool>>();

    for (const FieldSpec& field : fields)
    {
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text     = field.Label;
        label->FontSize = 13.0f;
        label->Color    = ZSlate::UIColor(0.75f, 0.78f, 0.85f, 1.0f);
        body->AddSlot(label);
        body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 2.0f)));

        if (field.Multiline)
        {
            auto area = std::make_shared<ZSlate::SMultiLineEditableText>();
            area->Options.FontSize  = 13.0f;
            area->Options.MinHeight = field.Height > 0.0f ? field.Height : 120.0f;
            area->Options.AutoWrap  = true;
            area->SetText(field.Value);
            body->AddSlot(area).Fill();
            multiBoxes->push_back(area);
        }
        else
        {
            auto box = std::make_shared<ZSlate::SEditableTextBox>();
            box->FontSize = 13.0f;
            box->MinWidth = field.MinWidth;
            box->SetText(field.Value);
            body->AddSlot(box);
            singleBoxes->push_back(box);
        }

        fieldIsMulti->push_back(field.Multiline);
        body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 8.0f)));
    }

    dialog->SetBody(body);

    auto buttons = std::make_shared<ZSlate::SHorizontalBox>();
    std::weak_ptr<ZSlate::SDialog> weakDialog = dialog;

    buttons->AddSlot(DlgButton("OK", [weakDialog, onOk, singleBoxes, multiBoxes, fieldIsMulti]() {
        std::vector<std::string> values;
        values.reserve(fieldIsMulti->size());
        size_t singleIndex = 0;
        size_t multiIndex  = 0;
        for (bool multiline : *fieldIsMulti)
        {
            if (multiline) values.push_back((*multiBoxes)[multiIndex++]->GetText());
            else           values.push_back((*singleBoxes)[singleIndex++]->Text);
        }
        if (onOk) onOk(values);
        if (auto dialog = weakDialog.lock()) dialog->Close();
    }));
    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(10.0f, 1.0f)));
    buttons->AddSlot(DlgButton("Cancel", [weakDialog]() {
        if (auto dialog = weakDialog.lock()) dialog->Close();
    }));
    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 1.0f)));

    dialog->SetButtons(buttons);
    dialog->Show();
}

} // namespace ClientProj
