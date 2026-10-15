#pragma once
// Forward declarations for all tabs
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include <memory>
#include <string>

namespace ClientProj {

inline std::shared_ptr<ZSlate::SWidget> MakePlaceholderTab(const std::string& title) {
    auto vbox = std::make_shared<ZSlate::SVerticalBox>();
    auto spacer = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 60));
    vbox->AddSlot(spacer);
    auto label = std::make_shared<ZSlate::STextBlock>();
    label->Text = title;
    label->FontSize = 24.0f;
    label->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    vbox->AddSlot(label);
    auto sub = std::make_shared<ZSlate::STextBlock>();
    sub->Text = "Coming soon...";
    sub->FontSize = 16.0f;
    sub->Color = ZSlate::UIColor(0.4f, 0.45f, 0.55f, 1.0f);
    vbox->AddSlot(sub);
    return vbox;
}

} // namespace ClientProj
