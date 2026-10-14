#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Application/SlateInput.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace ClientProj {

struct TabInfo {
    std::string Name;
    std::shared_ptr<ZSlate::SWidget> Content;
};

// Multi-tab application manager with a button-bar tab strip
class MainApp {
public:
    MainApp();

    void AddTab(const std::string& name, std::shared_ptr<ZSlate::SWidget> content);
    void SelectTab(int index);
    void SetInputRouter(ZSlate::SlateInputRouter* input) { m_Input = input; }

    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }

    void OnFrame();

private:
    void RebuildTabBar();

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::SHorizontalBox> m_TabBar;
    ZSlate::SlateInputRouter* m_Input = nullptr;

    std::vector<TabInfo> m_Tabs;
    int m_ActiveTab = -1;
};

} // namespace ClientProj
