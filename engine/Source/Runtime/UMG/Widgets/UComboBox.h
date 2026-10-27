#pragma once

#include "ZSlate/Widgets/Input/SComboBox.h"
#include "Runtime/UMG/Core/UWidget.h"

#include <functional>
#include <string>
#include <vector>

namespace ZUMG
{
// UE UMG UComboBox analogue -> ZSlate::SComboBox<std::string>. String options
// (the common case for authored HUDs); the label defaults to the string itself.
class UComboBox : public UWidget
{
public:
    std::vector<std::string> Options;
    int SelectedIndex {-1};
    std::string Placeholder {"..."};
    float Width {160.0f};
    float Height {26.0f};

    // Fired when the user commits a selection. index is the chosen row.
    std::function<void(const std::string&, int)> OnSelectionChanged;

    void SetOptions(std::vector<std::string> options)
    {
        Options = std::move(options);
        if (SelectedIndex >= static_cast<int>(Options.size()))
            SelectedIndex = -1;
        if (auto* w = GetSlateAs<ZSlate::SComboBox<std::string>>())
        {
            w->SetOptions(Options);
            w->SetSelectedIndex(SelectedIndex);
        }
    }
    void SetSelectedIndex(int index)
    {
        SelectedIndex = index;
        if (auto* w = GetSlateAs<ZSlate::SComboBox<std::string>>())
            w->SetSelectedIndex(index);
    }
    std::string GetSelected() const
    {
        if (auto* w = GetSlateAs<ZSlate::SComboBox<std::string>>())
            return w->SelectedItem;
        if (SelectedIndex >= 0 && SelectedIndex < static_cast<int>(Options.size()))
            return Options[static_cast<size_t>(SelectedIndex)];
        return std::string();
    }

    const char* GetWidgetClassName() const override { return "ComboBox"; }
    void SerializeProperties(UMGPropertyBag& bag) const override
    {
        UWidget::SerializeProperties(bag);
        // Options are joined with '\n' (option text is authored, so it should not
        // contain newlines; this keeps the bag to one scalar entry per key).
        std::string joined;
        for (size_t i = 0; i < Options.size(); ++i)
        {
            if (i) joined += '\n';
            joined += Options[i];
        }
        bag.SetString("options", joined);
        bag.SetInt("selected_index", SelectedIndex);
        bag.SetString("placeholder", Placeholder);
        bag.SetFloat("width", Width);
        bag.SetFloat("height", Height);
    }
    void DeserializeProperties(const UMGPropertyBag& bag) override
    {
        UWidget::DeserializeProperties(bag);
        Options.clear();
        const std::string joined = bag.GetString("options", std::string());
        if (!joined.empty())
        {
            size_t start = 0;
            while (start <= joined.size())
            {
                size_t end = joined.find('\n', start);
                if (end == std::string::npos)
                    end = joined.size();
                if (end > start)
                    Options.push_back(joined.substr(start, end - start));
                start = end + 1;
            }
        }
        SelectedIndex = bag.GetInt("selected_index", SelectedIndex);
        Placeholder = bag.GetString("placeholder", Placeholder);
        Width = bag.GetFloat("width", Width);
        Height = bag.GetFloat("height", Height);
    }

protected:
    std::shared_ptr<ZSlate::SWidget> RebuildWidget() override
    {
        auto combo = std::make_shared<ZSlate::SComboBox<std::string>>();
        combo->SetOptions(Options);
        combo->SetSelectedIndex(SelectedIndex);
        combo->Placeholder = Placeholder;
        combo->Size = ZSlate::Vector2(Width, Height);
        // Snapshot the callback (the SComboBox outlives this UButton-style wrapper;
        // do not capture `this`).
        auto handler = OnSelectionChanged;
        combo->OnSelectionChanged = [handler](const std::string& item, int32_t index)
        {
            if (handler)
                handler(item, static_cast<int>(index));
        };
        combo->Visibility = m_Visibility;
        return combo;
    }
};
}  // namespace ZUMG
