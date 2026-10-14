#include "MainApp.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Application/SlateApplication.h"

namespace ClientProj {

constexpr float TAB_BUTTON_HEIGHT = 32.0f;

MainApp::MainApp()
{
    m_Root = std::make_shared<ZSlate::SVerticalBox>();
    m_TabBar = std::make_shared<ZSlate::SHorizontalBox>();
    m_Root->AddSlot(m_TabBar);
}

void MainApp::AddTab(const std::string& name, std::shared_ptr<ZSlate::SWidget> content)
{
    m_Tabs.push_back({name, std::move(content)});
    RebuildTabBar();
    if (m_Tabs.size() == 1) SelectTab(0);
}

void MainApp::SelectTab(int index)
{
    if (index < 0 || index >= (int)m_Tabs.size()) return;
    m_ActiveTab = index;
    RebuildTabBar();

    // Replace content: remove all slots after tab bar, then add the active content
    auto newRoot = std::make_shared<ZSlate::SVerticalBox>();
    newRoot->AddSlot(m_TabBar);
    newRoot->AddSlot(m_Tabs[index].Content).Fill();
    m_Root = newRoot;

    ZSlate::SlateApplication::Get().SetRootContent(m_Root);

    // Reset input router — old widgets destroyed by tree rebuild
    if (m_Input) m_Input->Reset();
}

void MainApp::RebuildTabBar()
{
    // Rebuild tab bar buttons
    auto newBar = std::make_shared<ZSlate::SHorizontalBox>();

    for (int i = 0; i < (int)m_Tabs.size(); ++i) {
        auto btn = std::make_shared<ZSlate::SButton>();
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text = m_Tabs[i].Name;
        label->FontSize = 16.0f;
        label->Color = (i == m_ActiveTab) ? ZSlate::Colors::White
                                          : ZSlate::UIColor(0.6f, 0.65f, 0.75f, 1.0f);
        btn->SetContent(label);

        // Highlight active tab
        if (i == m_ActiveTab) {
            btn->NormalColor = ZSlate::UIColor(0.25f, 0.45f, 0.75f, 1.0f);
            btn->HoverColor  = ZSlate::UIColor(0.30f, 0.50f, 0.80f, 1.0f);
        } else {
            btn->NormalColor = ZSlate::UIColor(0.18f, 0.18f, 0.20f, 1.0f);
            btn->HoverColor  = ZSlate::UIColor(0.25f, 0.25f, 0.28f, 1.0f);
        }

        // Capture index by value
        int tabIndex = i;
        btn->OnClicked = [this, tabIndex]() { SelectTab(tabIndex); };
        newBar->AddSlot(btn);
    }

    // Fill remaining space
    auto spacer = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, TAB_BUTTON_HEIGHT));
    newBar->AddSlot(spacer);

    m_TabBar = newBar;
}

void MainApp::OnFrame() {}

} // namespace ClientProj
