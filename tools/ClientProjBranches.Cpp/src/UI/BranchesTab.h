#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/SWidget.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "SBranchTable.h"
#include "ContextMenuHelper.h"
#include "../Svn/SvnProbe.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "../Services/ExternalTools.h"
#include "../Services/IdeSettings.h"
#include "../Services/BranchPathsStore.h"
#include "../Services/GridLayoutStore.h"
#include "FieldDialog.h"
#include <Windows.h>
#include <shlobj_core.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

namespace ClientProj {

class BranchesTab {
public:
    BranchesTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
    void Refresh();

private:
    void BuildUI();
    void BuildPathManager();
    void TogglePathManager();
    void ClosePathManager(bool refreshBranches = false);
    void OnFilterChanged(const std::string& text);
    void OnRefresh();
    void OnOpenExplorer();
    void OnOpenIde();
    void OnSvnUpdate();
    void OnSvnLog();
    void OnManagePaths();
    void OnAddPath();
    void OnRemovePath();
    void OnAutoDetect();
    void OnBranchRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void OnRowDoubleClick(int32_t index);
    void SaveGridLayout();
    void OnPathRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void OnOpenTerminal();
    void OnCopyPath();
    void OpenPathInExplorer(const std::string& path);
    void OpenPathInTerminal(const std::string& path);
    void SavePaths();
    void LoadPaths();
    void RebuildPathList();
    void DoRebuild();
    void RenderGrid();

    std::shared_ptr<ZSlate::SBoxPanel> m_Root;
    std::shared_ptr<ZSlate::SBoxPanel> m_PathManagerPanel;
    std::shared_ptr<SBranchTable> m_BranchTable;
    std::shared_ptr<ZSlate::SListView<std::string>> m_PathListView;
    std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_PathDataSource;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;
    std::shared_ptr<ZSlate::STextBlock> m_PathStatus;
    std::shared_ptr<ZSlate::SEditableTextBox> m_FilterBox;

    std::vector<Svn::SvnItem> m_Items;
    std::vector<Svn::SvnItem> m_FilteredItems;
    std::vector<std::string> m_Paths;
    std::string m_FilterText;
    bool m_PathManagerVisible = false;
    bool m_NeedsRebuild = false;
    int m_PathSelectedIndex = -1;
    int m_BranchSelectedIndex = -1;

public:
    // Called from main loop — handle deferred root rebuilds, returns true if rebuild happened
    bool OnFrame();
};

// Helper
inline std::shared_ptr<ZSlate::SButton> BMakeBtn(const std::string& text, std::function<void()> cb,
                                                  float fontSize = 13.0f) {
    auto btn = std::make_shared<ZSlate::SButton>();
    btn->NormalColor  = ZSlate::UIColor(0.32f, 0.38f, 0.48f, 1.0f);
    btn->HoverColor   = ZSlate::UIColor(0.42f, 0.52f, 0.66f, 1.0f);
    btn->PressedColor = ZSlate::UIColor(0.22f, 0.28f, 0.38f, 1.0f);
    auto lbl = std::make_shared<ZSlate::STextBlock>();
    lbl->Text = text; lbl->FontSize = fontSize; lbl->Color = ZSlate::UIColor(0.96f, 0.97f, 1.0f, 1.0f);
    btn->SetContent(lbl);
    btn->OnClicked = std::move(cb);
    btn->Padding = {12.0f, 6.0f};
    return btn;
}

// Primary action button (slightly accented)
inline std::shared_ptr<ZSlate::SButton> BMakeBtnPrimary(const std::string& text, std::function<void()> cb,
                                                        float fontSize = 13.0f) {
    auto btn = BMakeBtn(text, std::move(cb), fontSize);
    btn->NormalColor  = ZSlate::UIColor(0.20f, 0.45f, 0.78f, 1.0f);
    btn->HoverColor   = ZSlate::UIColor(0.28f, 0.55f, 0.88f, 1.0f);
    btn->PressedColor = ZSlate::UIColor(0.16f, 0.38f, 0.65f, 1.0f);
    return btn;
}

// --- BranchesTab implementation ---

inline BranchesTab::BranchesTab()
    : m_Root(std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical))
{
    // Branch table with column headers
    m_BranchTable = std::make_shared<SBranchTable>();
    m_BranchTable->OnSelectionChanged = [this](const Svn::SvnItem&, int32_t idx) {
        m_BranchSelectedIndex = idx;
    };
    m_BranchTable->OnItemRightClicked = [this](int32_t index, const ZSlate::Vector2& pos) {
        OnBranchRightClick(index, pos);
    };

    // Double-click a row opens the working copy folder (C# CellDoubleClick).
    m_BranchTable->OnItemDoubleClicked = [this](int32_t index) {
        OnRowDoubleClick(index);
    };

    // Column layout: restore the persisted order/widths, then persist again
    // after every resize or reorder (C# GridLayoutBinder).
    if (auto header = m_BranchTable->GetHeaderRow()) {
        header->ApplyLayout(GridLayout::OrderedWidths("BranchesTab"));
        header->OnColumnsChanged = [this]() { SaveGridLayout(); };
    }

    // Path editor list
    m_PathDataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
    m_PathListView = std::make_shared<ZSlate::SListView<std::string>>();
    m_PathListView->SetDataSource(m_PathDataSource);
    m_PathListView->Options.ItemHeight = 26.0f;
    m_PathListView->SetOnGenerateItemWidget([](const std::string& item, int32_t, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 14.0f;
        w->Color = ZSlate::UIColor(0.85f, 0.88f, 0.92f, 1.0f);
        return w;
    });
    m_PathListView->SetOnSelectionChanged([this](const auto& sel) {
        m_PathSelectedIndex = sel.empty() ? -1 : sel[0];
    });
    m_PathListView->SetOnItemRightClicked([this](int32_t index, const ZSlate::Vector2& pos) {
        OnPathRightClick(index, pos);
    });

    BuildUI();
    Refresh();
}

inline void BranchesTab::BuildUI() {
    // ---- Top bar: Filter ----
    auto topBar = std::make_shared<ZSlate::SHorizontalBox>();
    auto filterLabel = std::make_shared<ZSlate::STextBlock>();
    filterLabel->Text = "Filter:"; filterLabel->FontSize = 14.0f;
    filterLabel->Color = ZSlate::UIColor(0.75f, 0.80f, 0.90f, 1.0f);
    topBar->AddSlot(filterLabel);
    topBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));

    m_FilterBox = std::make_shared<ZSlate::SEditableTextBox>();
    m_FilterBox->MinWidth = 280.0f;
    m_FilterBox->FontSize = 14.0f;
    m_FilterBox->OnTextChanged = [this](const std::string& text) { OnFilterChanged(text); };
    topBar->AddSlot(m_FilterBox);
    topBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));

    auto hint = std::make_shared<ZSlate::STextBlock>();
    hint->Text = "Right-click a row for actions. Double-click opens folder.";
    hint->FontSize = 12.0f;
    hint->Color = ZSlate::UIColor(0.55f, 0.60f, 0.70f, 1.0f);
    topBar->AddSlot(hint);
    m_Root->AddSlot(topBar);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 6)));

    // Toolbar
    auto toolbar = std::make_shared<ZSlate::SHorizontalBox>();
    toolbar->AddSlot(BMakeBtnPrimary("Refresh",      [this]() { OnRefresh(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6, 1)));
    toolbar->AddSlot(BMakeBtn("Explorer",     [this]() { OnOpenExplorer(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6, 1)));
    toolbar->AddSlot(BMakeBtn("IDE",          [this]() { OnOpenIde(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6, 1)));
    toolbar->AddSlot(BMakeBtn("SVN Update",   [this]() { OnSvnUpdate(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6, 1)));
    toolbar->AddSlot(BMakeBtn("SVN Log",      [this]() { OnSvnLog(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6, 1)));
    toolbar->AddSlot(BMakeBtn("Commit",       [this]() {
        if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size())
            Tools::OpenTortoiseProc("commit", m_FilteredItems[m_BranchSelectedIndex].Path);
    }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    toolbar->AddSlot(BMakeBtn(m_PathManagerVisible ? "Close Paths" : "Manage Paths",
                               [this]() { OnManagePaths(); }));
    m_Root->AddSlot(toolbar);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 6)));

    // Path manager panel (only when visible)
    if (m_PathManagerVisible) {
        BuildPathManager();
        m_Root->AddSlot(m_PathManagerPanel);
    } else {
        // Create empty placeholder so the pointer is non-null and DoRebuild can replace it
        m_PathManagerPanel = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);
    }

    // Branch table
    m_Root->AddSlot(m_BranchTable);

    // Status bar
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_StatusLabel = std::make_shared<ZSlate::STextBlock>();
    m_StatusLabel->Text = "Ready";
    m_StatusLabel->FontSize = 13.0f;
    m_StatusLabel->Color = ZSlate::UIColor(0.70f, 0.75f, 0.85f, 1.0f);
    m_Root->AddSlot(m_StatusLabel);
}

inline void BranchesTab::BuildPathManager() {
    m_PathManagerPanel = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

    auto titleBar = std::make_shared<ZSlate::SHorizontalBox>();
    auto title = std::make_shared<ZSlate::STextBlock>();
    title->Text = "SVN Scan Paths";
    title->FontSize = 16.0f;
    title->Color = ZSlate::UIColor(0.95f, 0.97f, 1.0f, 1.0f);
    titleBar->AddSlot(title);
    titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    titleBar->AddSlot(BMakeBtn("Add Folder...", [this]() { OnAddPath(); }));
    titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    titleBar->AddSlot(BMakeBtn("Remove",        [this]() { OnRemovePath(); }));
    titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    titleBar->AddSlot(BMakeBtn("Auto-Detect",   [this]() { OnAutoDetect(); }));
    titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    titleBar->AddSlot(BMakeBtn("Save && Close",  [this]() { ClosePathManager(true); }));
    titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    titleBar->AddSlot(BMakeBtn("Cancel",         [this]() { ClosePathManager(false); }));
    m_PathManagerPanel->AddSlot(titleBar);
    m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    auto hint = std::make_shared<ZSlate::STextBlock>();
    hint->Text = "  Directories to scan for SVN working copies (e.g. D:\\OSGame\\Client_proj)";
    hint->FontSize = 13.0f;
    hint->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_PathManagerPanel->AddSlot(hint);
    m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    m_PathManagerPanel->AddSlot(m_PathListView);
    m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    m_PathStatus = std::make_shared<ZSlate::STextBlock>();
    m_PathStatus->Text = "";
    m_PathStatus->FontSize = 13.0f;
    m_PathStatus->Color = ZSlate::UIColor(0.45f, 0.5f, 0.6f, 1.0f);
    m_PathManagerPanel->AddSlot(m_PathStatus);

    auto sep = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 6));
    m_PathManagerPanel->AddSlot(sep);
}

inline void BranchesTab::DoRebuild() {
    auto newRoot = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

    // Top bar: Filter
    auto topBar = std::make_shared<ZSlate::SHorizontalBox>();
    auto filterLabel = std::make_shared<ZSlate::STextBlock>();
    filterLabel->Text = "Filter:"; filterLabel->FontSize = 14.0f;
    filterLabel->Color = ZSlate::UIColor(0.6f, 0.65f, 0.75f, 1.0f);
    topBar->AddSlot(filterLabel);

    m_FilterBox = std::make_shared<ZSlate::SEditableTextBox>();
    m_FilterBox->MinWidth = 240.0f;
    m_FilterBox->FontSize = 14.0f;
    m_FilterBox->SetText(m_FilterText);
    m_FilterBox->OnTextChanged = [this](const std::string& text) { OnFilterChanged(text); };
    topBar->AddSlot(m_FilterBox);
    topBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));

    auto hint = std::make_shared<ZSlate::STextBlock>();
    hint->Text = "Right-click a row for actions. Double-click opens folder.";
    hint->FontSize = 12.0f;
    hint->Color = ZSlate::UIColor(0.45f, 0.5f, 0.6f, 1.0f);
    topBar->AddSlot(hint);
    newRoot->AddSlot(topBar);
    newRoot->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    auto toolbar = std::make_shared<ZSlate::SHorizontalBox>();
    toolbar->AddSlot(BMakeBtn("Refresh",      [this]() { OnRefresh(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    toolbar->AddSlot(BMakeBtn("Explorer",     [this]() { OnOpenExplorer(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    toolbar->AddSlot(BMakeBtn("IDE",          [this]() { OnOpenIde(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    toolbar->AddSlot(BMakeBtn("SVN Update",   [this]() { OnSvnUpdate(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    toolbar->AddSlot(BMakeBtn("SVN Log",      [this]() { OnSvnLog(); }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    toolbar->AddSlot(BMakeBtn("Commit",       [this]() {
        if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size())
            Tools::OpenTortoiseProc("commit", m_FilteredItems[m_BranchSelectedIndex].Path);
    }));
    toolbar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    toolbar->AddSlot(BMakeBtn(m_PathManagerVisible ? "Close Paths" : "Manage Paths",
                               [this]() { OnManagePaths(); }));
    newRoot->AddSlot(toolbar);
    newRoot->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    if (m_PathManagerVisible) {
        m_PathManagerPanel = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);
        auto titleBar = std::make_shared<ZSlate::SHorizontalBox>();
        auto title = std::make_shared<ZSlate::STextBlock>();
        title->Text = "SVN Scan Paths"; title->FontSize = 16.0f;
        title->Color = ZSlate::UIColor(0.95f, 0.97f, 1.0f, 1.0f);
        titleBar->AddSlot(title);
        titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
        titleBar->AddSlot(BMakeBtn("Add Folder...", [this]() { OnAddPath(); }));
        titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
        titleBar->AddSlot(BMakeBtn("Remove",        [this]() { OnRemovePath(); }));
        titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
        titleBar->AddSlot(BMakeBtn("Auto-Detect",   [this]() { OnAutoDetect(); }));
        titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
        titleBar->AddSlot(BMakeBtn("Save && Close",  [this]() { ClosePathManager(true); }));
        titleBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
        titleBar->AddSlot(BMakeBtn("Cancel",         [this]() { ClosePathManager(false); }));
        m_PathManagerPanel->AddSlot(titleBar);
        m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
        auto hint = std::make_shared<ZSlate::STextBlock>();
        hint->Text = "  Directories to scan for SVN working copies (e.g. D:\\OSGame\\Client_proj)";
        hint->FontSize = 13.0f; hint->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
        m_PathManagerPanel->AddSlot(hint);
        m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
        m_PathManagerPanel->AddSlot(m_PathListView);
        m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
        m_PathStatus = std::make_shared<ZSlate::STextBlock>();
        m_PathStatus->Text = std::to_string(m_Paths.size()) + " path(s)";
        m_PathStatus->FontSize = 13.0f; m_PathStatus->Color = ZSlate::UIColor(0.45f, 0.5f, 0.6f, 1.0f);
        m_PathManagerPanel->AddSlot(m_PathStatus);
        m_PathManagerPanel->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 6)));
        newRoot->AddSlot(m_PathManagerPanel);
    }

    newRoot->AddSlot(m_BranchTable);
    newRoot->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    newRoot->AddSlot(m_StatusLabel);

    m_Root = newRoot;
    ZSlate::SlateApplication::Get().SetRootContent(m_Root);
    m_NeedsRebuild = false;
}

inline bool BranchesTab::OnFrame() {
    if (m_NeedsRebuild) { DoRebuild(); return true; }
    return false;
}

inline void BranchesTab::OnFilterChanged(const std::string& text) {
    m_FilterText = text;
    RenderGrid();
}

inline void BranchesTab::RenderGrid() {
    m_FilteredItems.clear();
    std::string needle = m_FilterText;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    for (auto& item : m_Items) {
        if (!needle.empty()) {
            std::string hay = item.Drive + " " + item.BranchType + " " + item.BranchName + " "
                            + item.RelativeUrl + " " + item.Path + " " + item.Url;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        m_FilteredItems.push_back(item);
    }

    m_BranchTable->SetItems(m_FilteredItems);
    m_StatusLabel->Text = "Visible: " + std::to_string(m_FilteredItems.size())
                        + " / Total: " + std::to_string(m_Items.size());
}

inline void BranchesTab::TogglePathManager() {
    m_PathManagerVisible = !m_PathManagerVisible;
    if (m_PathManagerVisible) {
        LoadPaths();
        RebuildPathList();
    }
    m_NeedsRebuild = true;
}

inline void BranchesTab::ClosePathManager(bool refreshBranches) {
    SavePaths();
    TogglePathManager();
    if (refreshBranches) Refresh();
}

inline void BranchesTab::OnManagePaths() {
    TogglePathManager();
}

inline void BranchesTab::Refresh() {
    m_Paths.clear();
    m_Items.clear();

    // paths.json (bare array, C#-compatible); fall back to a drive scan when
    // nothing is configured yet, exactly like BranchPathsStore.CandidatePaths().
    m_Paths = BranchPathsStore::Load();
    if (m_Paths.empty()) m_Paths = BranchPathsStore::AutoScan();

    for (const auto& dir : m_Paths) {
        auto roots = Svn::FindSvnRoots(dir);
        for (auto& root : roots) {
            m_Items.push_back(Svn::GetSvnInfo(root.Path));
        }
    }

    RenderGrid();
}

inline void BranchesTab::OnRefresh() { Refresh(); }

inline void BranchesTab::OnOpenExplorer() {
    if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size()) {
        Tools::OpenExplorer(m_FilteredItems[m_BranchSelectedIndex].Path);
        m_StatusLabel->Text = "Opened: " + m_FilteredItems[m_BranchSelectedIndex].Path;
    } else {
        m_StatusLabel->Text = "Select a branch first";
    }
}

inline void BranchesTab::OnOpenIde() {
    if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size()) {
        Tools::OpenInIde(m_FilteredItems[m_BranchSelectedIndex].Path);
        m_StatusLabel->Text = "Opening in IDE: " + m_FilteredItems[m_BranchSelectedIndex].Path;
    } else {
        m_StatusLabel->Text = "Select a branch first";
    }
}

inline void BranchesTab::OnSvnUpdate() {
    if (m_BranchSelectedIndex < 0 || m_BranchSelectedIndex >= (int32_t)m_FilteredItems.size()) return;

    const std::string path = m_FilteredItems[m_BranchSelectedIndex].Path;
    if (Tools::RunTortoiseVerb(path, "update")) {
        m_StatusLabel->Text = "SVN Update: " + path;
        Refresh();
    } else {
        m_StatusLabel->Text = "SVN Update failed — working copy or svn.exe / TortoiseSVN missing: " + path;
    }
}

inline void BranchesTab::OnSvnLog() {
    if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size()) {
        Tools::OpenTortoiseLog(m_FilteredItems[m_BranchSelectedIndex].Path);
        m_StatusLabel->Text = "Showing log for: " + m_FilteredItems[m_BranchSelectedIndex].BranchName;
    }
}

inline void BranchesTab::OnAddPath() {
    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select a directory to scan for SVN working copies";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char path[MAX_PATH];
    SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);

    std::string spath(path);
    for (auto& p : m_Paths) if (p == spath) { m_PathStatus->Text = "Already in list"; return; }
    m_Paths.push_back(spath);
    RebuildPathList();
    m_PathStatus->Text = "Added: " + spath;
}

inline void BranchesTab::OnRemovePath() {
    if (m_PathSelectedIndex < 0 || m_PathSelectedIndex >= (int)m_Paths.size()) {
        m_PathStatus->Text = "Select a path to remove";
        return;
    }
    auto removed = m_Paths[m_PathSelectedIndex];
    m_Paths.erase(m_Paths.begin() + m_PathSelectedIndex);
    m_PathSelectedIndex = -1;
    RebuildPathList();
    m_PathStatus->Text = "Removed: " + removed;
}

inline void BranchesTab::OnAutoDetect() {
    int added = 0;
    DWORD drives = GetLogicalDrives();
    for (char d = 'C'; d <= 'Z'; d++) {
        if (!(drives & (1 << (d - 'A')))) continue;
        std::string path = std::string(1, d) + ":\\OSGame\\Client_proj";
        if (!std::filesystem::exists(path)) continue;
        bool exists = false;
        for (auto& p : m_Paths) if (p == path) { exists = true; break; }
        if (!exists) { m_Paths.push_back(path); added++; }
    }
    RebuildPathList();
    m_PathStatus->Text = "Auto-detected " + std::to_string(added) + " new path(s)";
}

inline void BranchesTab::SavePaths() {
    // Bare JSON array — identical to C# BranchPathsStore.
    BranchPathsStore::Save(m_Paths);
}

inline void BranchesTab::LoadPaths() {
    m_Paths = BranchPathsStore::Load();
}

inline void BranchesTab::RebuildPathList() {
    std::vector<std::string> display;
    for (auto& p : m_Paths) display.push_back(p);
    if (display.empty()) display.push_back("(no paths configured)");
    m_PathDataSource->SetItems(display);
}

// ---- Right-click context menu via Win32 TrackPopupMenu ----

namespace {
    void OpenInTerminal(const std::string& path) {
        std::string cmd = "start cmd.exe /K \"cd /d " + path + "\"";
        system(cmd.c_str());
    }
} // namespace

inline void BranchesTab::OnBranchRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_FilteredItems.size()) return;

    HWND hWnd = GetActiveWindow();
    // Rich context menu matching C# version
    std::vector<std::pair<std::string, int>> items = {
        {"Open Folder",       1},
        {"Open in Terminal",  2},
        {"Open in IDE",       3},
        {"-",                 0},
        {"Open Unity",        10},
        {"Kill Unity",        11},
        {"-",                 0},
        {"SVN Update",        4},
        {"SVN Update to Revision...", 20},
        {"SVN Commit",        21},
        {"SVN Switch",        22},
        {"SVN Merge",         23},
        {"SVN Show Log",      5},
        {"SVN Revert",        24},
        {"SVN Cleanup",       25},
        {"-",                 0},
        {"Copy Path",         6},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);
    auto& item = m_FilteredItems[index];

    switch (cmd) {
    case 1: // Open in Explorer
        Tools::OpenExplorer(item.Path);
        m_StatusLabel->Text = "Opened: " + item.Path;
        break;
    case 2: // Open in Terminal
        OpenInTerminal(item.Path);
        break;
    case 3: // Open in IDE
        Tools::OpenInIde(item.Path);
        m_StatusLabel->Text = "Opening in IDE: " + item.Path;
        break;
    case 10: // Open Unity
        {
            std::string unityPath = item.Path + "\\UnityProject";
            if (std::filesystem::exists(unityPath))
                Tools::OpenExplorer(unityPath);
            else
                m_StatusLabel->Text = "UnityProject folder not found";
        }
        break;
    case 11: // Kill Unity
        Tools::KillProcess("Unity.exe");
        m_StatusLabel->Text = "Unity killed";
        break;
    case 4: // SVN Update
        if (Tools::RunTortoiseVerb(item.Path, "update")) {
            m_StatusLabel->Text = "SVN Update: " + item.Path;
            Refresh();
        } else {
            m_StatusLabel->Text = "SVN Update failed — working copy or svn.exe / TortoiseSVN missing: " + item.Path;
        }
        break;
    case 20: // SVN Update to Revision
        {
            // Port of the C# UpdateToRevisionDialog.
            const std::string path = item.Path;
            ShowFieldsDialog("SVN Update to Revision", {
                { "Revision (positive number, or HEAD / BASE / PREV / COMMITTED)", "HEAD", false, 380.0f, 0.0f },
            }, [this, path](const std::vector<std::string>& values) {
                const std::string revision = values.empty() ? "" : values[0];
                if (!Tools::IsValidSvnRevision(revision)) {
                    m_StatusLabel->Text = "Invalid revision: " + revision;
                    return;
                }
                if (Tools::RunTortoiseVerb(path, "update", "/rev:" + revision, "update -r " + revision)) {
                    m_StatusLabel->Text = "SVN Update to " + revision + ": " + path;
                    Refresh();
                } else {
                    m_StatusLabel->Text = "SVN Update to " + revision + " failed: " + path;
                }
            });
        }
        break;
    case 21: // SVN Commit
        Tools::OpenTortoiseProc("commit", item.Path);
        m_StatusLabel->Text = "Commit dialog for: " + item.BranchName;
        break;
    case 22: // SVN Switch
        Tools::OpenTortoiseProc("switch", item.Path);
        m_StatusLabel->Text = "Switch dialog for: " + item.BranchName;
        break;
    case 23: // SVN Merge
        Tools::OpenTortoiseProc("merge", item.Path);
        m_StatusLabel->Text = "Merge dialog for: " + item.BranchName;
        break;
    case 5: // SVN Show Log
        Tools::OpenTortoiseLog(item.Path);
        m_StatusLabel->Text = "Showing log for: " + item.BranchName;
        break;
    case 24: // SVN Revert
        Tools::OpenTortoiseProc("revert", item.Path);
        m_StatusLabel->Text = "Revert dialog for: " + item.BranchName;
        break;
    case 25: // SVN Cleanup
        if (Tools::RunTortoiseVerb(item.Path, "cleanup")) {
            m_StatusLabel->Text = "SVN Cleanup: " + item.Path;
            Refresh();
        } else {
            m_StatusLabel->Text = "SVN Cleanup failed — working copy or svn.exe / TortoiseSVN missing: " + item.Path;
        }
        break;
    case 6: // Copy Path
        CopyTextToClipboard(item.Path);
        m_StatusLabel->Text = "Copied: " + item.Path;
        break;
    }
}

inline void BranchesTab::OnRowDoubleClick(int32_t index) {
    if (index < 0 || index >= (int32_t)m_FilteredItems.size()) return;

    const std::string path = m_FilteredItems[index].Path;
    Tools::OpenExplorer(path);
    m_StatusLabel->Text = "Opened: " + path;
}

inline void BranchesTab::SaveGridLayout() {
    auto header = m_BranchTable->GetHeaderRow();
    if (!header) return;

    std::vector<std::pair<std::string, float>> columns;
    columns.reserve(header->Columns.size());
    for (const auto& column : header->Columns)
        columns.emplace_back(column.Name, column.Width);

    GridLayout::Save("BranchesTab", columns);
    m_StatusLabel->Text = "Column layout saved";
}

inline void BranchesTab::OnPathRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_Paths.size()) return;

    HWND hWnd = GetActiveWindow();
    std::vector<std::pair<std::string, int>> items = {
        {"Open in Explorer",  1},
        {"Open in Terminal",  2},
        {"-",                 0},
        {"Remove",            3},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);
    auto& path = m_Paths[index];

    switch (cmd) {
    case 1:
        Tools::OpenExplorer(path);
        break;
    case 2:
        OpenInTerminal(path);
        break;
    case 3:
        OnRemovePath();
        break;
    }
}

inline void BranchesTab::OnOpenTerminal() {
    if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size()) {
        OpenInTerminal(m_FilteredItems[m_BranchSelectedIndex].Path);
    }
}

inline void BranchesTab::OnCopyPath() {
    if (m_BranchSelectedIndex >= 0 && m_BranchSelectedIndex < (int32_t)m_FilteredItems.size()) {
        CopyTextToClipboard(m_FilteredItems[m_BranchSelectedIndex].Path);
        m_StatusLabel->Text = "Copied: " + m_FilteredItems[m_BranchSelectedIndex].Path;
    }
}

inline void BranchesTab::OpenPathInExplorer(const std::string& path) {
    Tools::OpenExplorer(path);
}

inline void BranchesTab::OpenPathInTerminal(const std::string& path) {
    OpenInTerminal(path);
}

} // namespace ClientProj
