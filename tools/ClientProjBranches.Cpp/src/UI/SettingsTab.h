#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Text/SMultiLineEditableText.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "../Services/IdeSettings.h"
#include "../Services/ExternalTools.h"
#include "../Services/BranchPathsStore.h"
#include <memory>
#include <string>
#include <vector>

namespace ClientProj {

class SettingsTab {
public:
    SettingsTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
private:
    void BuildUI();
    void OnSelectIde(const std::string& ide);
    void OnSavePaths();
    void OnAutoDetectPaths();
    void LoadPaths();

    static std::vector<std::string> SplitLines(const std::string& text);

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::STextBlock> m_IdeStatus, m_PathStatus;
    std::shared_ptr<ZSlate::SMultiLineEditableText> m_PathEditor;
};

inline std::shared_ptr<ZSlate::SButton> SMkBtn(const std::string& t, std::function<void()> cb) {
    auto b = std::make_shared<ZSlate::SButton>();
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = 14.0f; l->Color = ZSlate::Colors::White;
    b->SetContent(l); b->OnClicked = std::move(cb); return b;
}
inline std::shared_ptr<ZSlate::STextBlock> SMkLbl(const std::string& t, float s = 14.0f) {
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = s;
    l->Color = ZSlate::UIColor(0.85f, 0.88f, 0.92f, 1.0f);
    return l;
}

inline SettingsTab::SettingsTab() : m_Root(std::make_shared<ZSlate::SVerticalBox>()) { BuildUI(); }

inline void SettingsTab::BuildUI() {
    m_Root->AddSlot(SMkLbl("IDE Preference", 16.0f));
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    auto ideBox = std::make_shared<ZSlate::SHorizontalBox>();
    ideBox->AddSlot(SMkBtn("CodeBuddy", [this]() { OnSelectIde("codebuddy"); }));
    ideBox->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    ideBox->AddSlot(SMkBtn("Cursor", [this]() { OnSelectIde("cursor"); }));
    m_Root->AddSlot(ideBox);
    m_IdeStatus = SMkLbl("Current: " + IdeSettings::Load(), 13.0f);
    m_IdeStatus->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_IdeStatus);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 16)));

    m_Root->AddSlot(SMkLbl("SVN Branch Scan Paths", 16.0f));
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(SMkLbl("Enter directories to scan (one per line):", 12.0f));

    m_PathEditor = std::make_shared<ZSlate::SMultiLineEditableText>();
    m_PathEditor->Options.FontSize = 14.0f;
    m_PathEditor->Options.MinHeight = 120.0f;
    m_PathEditor->Options.ReadOnly = false;
    m_Root->AddSlot(m_PathEditor);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    auto pathBtns = std::make_shared<ZSlate::SHorizontalBox>();
    pathBtns->AddSlot(SMkBtn("Save Paths", [this]() { OnSavePaths(); }));
    pathBtns->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    pathBtns->AddSlot(SMkBtn("Reload", [this]() { LoadPaths(); }));
    pathBtns->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    pathBtns->AddSlot(SMkBtn("Auto-Detect", [this]() { OnAutoDetectPaths(); }));
    m_Root->AddSlot(pathBtns);

    m_PathStatus = SMkLbl("", 13.0f);
    m_PathStatus->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_PathStatus);

    LoadPaths();
}

inline void SettingsTab::OnSelectIde(const std::string& ide) {
    IdeSettings::Save(ide);
    m_IdeStatus->Text = "Current: " + IdeSettings::CanonicalName(ide);
}

inline std::vector<std::string> SettingsTab::SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos) ? std::string::npos : nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

inline void SettingsTab::LoadPaths() {
    std::string text;
    for (const auto& path : BranchPathsStore::Load()) {
        if (!text.empty()) text += "\n";
        text += path;
    }
    m_PathEditor->SetText(text);
}

inline void SettingsTab::OnSavePaths() {
    const std::vector<std::string> paths = SplitLines(m_PathEditor->GetText());
    BranchPathsStore::Save(paths);
    m_PathStatus->Text = "Saved " + std::to_string(paths.size()) + " path(s)";
}

inline void SettingsTab::OnAutoDetectPaths() {
    const auto found = BranchPathsStore::AutoScan();
    std::string text;
    for (const auto& path : found) {
        if (!text.empty()) text += "\n";
        text += path;
    }
    m_PathEditor->SetText(text);
    m_PathStatus->Text = "Auto-detected " + std::to_string(found.size())
                       + " path(s) — press 'Save Paths' to keep them";
}

}
