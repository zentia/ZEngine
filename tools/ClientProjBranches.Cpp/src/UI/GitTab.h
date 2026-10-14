#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "../Git/GitProbe.h"
#include "../Services/ExternalTools.h"
#include "../Services/GitTagStore.h"
#include "ContextMenuHelper.h"
#include "ZSlate/Widgets/Dialogs/SDialog.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <shlobj_core.h>
#include <commdlg.h>
#include <fstream>

namespace ClientProj {

struct GitRepoEntry { std::string Name, Path, Tag; };

class GitTab {
public:
    GitTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
    void Refresh();

private:
    void BuildUI();
    void LoadReposFromDisk();
    void SaveReposToDisk();
    void OnAddRepo();
    void OnRemoveRepo();
    void OnRefresh();
    void OnFetchAll();
    void OnOpenFolder();
    void OnOpenTerminal();
    void OnOpenIde();
    void OnGitPull();
    void OnGitPush();
    void OnGitFetch();
    void OnGitStatus();
    void OnUgitLog();
    void OnUgitOpen();
    void OnCopyPath();
    void OnRepoRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void RebuildList();

    // ---- Tags (port of the C# GitTagStore / EditTagDialog flow) ------------
    void LoadTagStylesFromDisk();
    void PersistTagStylesToDisk();
    void PruneOrphanTagStyles();
    void RegroupByTag();
    void OnSetTag();
    void ShowSetTagDialog(int index);

    ZSlate::UIColor RowColorFor(int index) const;
    std::string     FormatRow(int index) const;

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::SListView<std::string>> m_ListView;
    std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_DataSource;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;
    std::vector<GitRepoEntry> m_Repos;
    std::map<std::string, GitProbe::RepoSummary> m_SummaryByPath;
    std::map<std::string, GitTags::GitTagStyle>  m_TagStyles;   // key = lowercase tag
    std::string m_GitExe;
    std::string m_UgitPath;
    int m_SelectedIndex = -1;
};

// Win32 colour picker; returns false when the user cancels. `argb` is
// 0xAARRGGBB, 0 meaning "unset" (matches the C# ForeArgb/BackArgb sentinel).
inline bool GPickColor(HWND owner, int& argb) {
    static COLORREF customColors[16] = {};
    CHOOSECOLORA cc {};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.lpCustColors = customColors;
    cc.Flags        = CC_FULLOPEN | CC_ANYCOLOR;
    if (argb != 0) {
        cc.Flags |= CC_RGBINIT;
        cc.rgbResult = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    }
    if (!ChooseColorA(&cc)) return false;
    argb = 0xFF000000
         | (static_cast<int>(GetRValue(cc.rgbResult)) << 16)
         | (static_cast<int>(GetGValue(cc.rgbResult)) << 8)
         | (static_cast<int>(GetBValue(cc.rgbResult)));
    return true;
}

inline ZSlate::UIColor GArgbToColor(int argb) {
    return ZSlate::UIColor(static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                           static_cast<float>((argb >> 8)  & 0xFF) / 255.0f,
                           static_cast<float>( argb        & 0xFF) / 255.0f,
                           1.0f);
}

inline std::string GArgbToHex(int argb) {
    if (argb == 0) return "Default";
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return buf;
}

// Ordinal-ignore-case comparison (the C# side sorts with StringComparer.OrdinalIgnoreCase).
inline int GCompareCI(const std::string& a, const std::string& b) {
    const size_t n = (a.size() < b.size()) ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i]));
        const int cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

inline std::shared_ptr<ZSlate::SButton> GMakeBtn(const std::string& t, std::function<void()> cb) {
    auto b = std::make_shared<ZSlate::SButton>();
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = 14.0f; l->Color = ZSlate::Colors::White;
    b->SetContent(l); b->OnClicked = std::move(cb);
    return b;
}

inline GitTab::GitTab() : m_Root(std::make_shared<ZSlate::SVerticalBox>()) {
    m_GitExe = GitProbe::ResolveGitExe();
    m_UgitPath = Tools::ResolveUgit();
    m_DataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
    m_ListView = std::make_shared<ZSlate::SListView<std::string>>();
    m_ListView->SetDataSource(m_DataSource);
    m_ListView->Options.ItemHeight = 26.0f;
    m_ListView->SetOnGenerateItemWidget([this](const std::string& item, int32_t index, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 13.0f;
        w->Color = RowColorFor(static_cast<int>(index));
        return w;
    });
    // C# GitBranchesTab.CellDoubleClick opens the folder.
    m_ListView->SetOnItemDoubleClicked([this](int32_t) { OnOpenFolder(); });

    m_ListView->SetOnSelectionChanged([this](const auto& sel) {
        m_SelectedIndex = sel.empty() ? -1 : sel[0];
    });
    m_ListView->SetOnItemRightClicked([this](int32_t index, const ZSlate::Vector2& pos) {
        OnRepoRightClick(index, pos);
    });
    BuildUI();
    LoadTagStylesFromDisk();
    LoadReposFromDisk();
    RebuildList();
}

inline void GitTab::BuildUI() {
    auto tb = std::make_shared<ZSlate::SHorizontalBox>();
    tb->AddSlot(GMakeBtn("Add Repo...",  [this]() { OnAddRepo(); }));
    tb->AddSlot(GMakeBtn("Remove",       [this]() { OnRemoveRepo(); }));
    tb->AddSlot(GMakeBtn("Refresh",      [this]() { OnRefresh(); }));
    tb->AddSlot(GMakeBtn("Fetch All",    [this]() { OnFetchAll(); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    tb->AddSlot(GMakeBtn("Open Folder",  [this]() { OnOpenFolder(); }));
    tb->AddSlot(GMakeBtn("Terminal",     [this]() { OnOpenTerminal(); }));
    tb->AddSlot(GMakeBtn("IDE",          [this]() { OnOpenIde(); }));
    tb->AddSlot(GMakeBtn("Git Pull",     [this]() { OnGitPull(); }));
    tb->AddSlot(GMakeBtn("Git Push",     [this]() { OnGitPush(); }));
    tb->AddSlot(GMakeBtn("UGit Log",     [this]() { OnUgitLog(); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(10, 1)));
    tb->AddSlot(GMakeBtn("Set Tag...",   [this]() { OnSetTag(); }));
    m_Root->AddSlot(tb);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_ListView);
    m_StatusLabel = std::make_shared<ZSlate::STextBlock>();
    m_StatusLabel->Text = "Ready"; m_StatusLabel->FontSize = 13.0f;
    m_StatusLabel->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_StatusLabel);
}

inline void GitTab::LoadReposFromDisk() {
    m_Repos.clear();

    // C# writes a bare array of {"Name","Path","Tag"}.
    for (const auto& value : Json::LoadArrayFromFile(Paths::GitRepos())) {
        if (!value || !value->IsObject()) continue;
        const Json::Object& object = value->AsObject();
        GitRepoEntry entry;
        entry.Name = Json::OptStringCI(object, "Name");
        entry.Path = Json::OptStringCI(object, "Path");
        entry.Tag  = Json::OptStringCI(object, "Tag");
        if (entry.Path.empty()) continue;
        if (entry.Name.empty()) entry.Name = fs::path(entry.Path).filename().string();
        m_Repos.push_back(entry);
    }

    if (!m_Repos.empty()) return;

    // Legacy C++ shape: {"repos":[{"name","path","tag"}]}
    const Json::Object legacy = Json::LoadFromFile(Paths::GitRepos());
    if (const Json::Array* array = Json::OptArray(legacy, "repos")) {
        for (const auto& value : *array) {
            if (!value || !value->IsObject()) continue;
            const Json::Object& object = value->AsObject();
            GitRepoEntry entry;
            entry.Name = Json::OptStringCI(object, "Name");
            entry.Path = Json::OptStringCI(object, "Path");
            entry.Tag  = Json::OptStringCI(object, "Tag");
            if (entry.Path.empty()) continue;
            if (entry.Name.empty()) entry.Name = fs::path(entry.Path).filename().string();
            m_Repos.push_back(entry);
        }
    }
}

inline void GitTab::SaveReposToDisk() {
    // Bare array, PascalCase keys, de-duplicated by path — matches C# GitRepoStore.
    Json::Array array;
    std::vector<std::string> seen;
    for (const GitRepoEntry& repo : m_Repos) {
        if (repo.Path.empty()) continue;
        std::string key = repo.Path;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);

        Json::Object object;
        object["Name"] = std::make_shared<Json::Value>(repo.Name);
        object["Path"] = std::make_shared<Json::Value>(repo.Path);
        object["Tag"]  = std::make_shared<Json::Value>(repo.Tag);
        array.push_back(std::make_shared<Json::Value>(object));
    }
    Json::SaveArrayToFile(Paths::GitRepos(), array);
}

inline void GitTab::OnAddRepo() {
    BROWSEINFOA bi = {};
    bi.lpszTitle = "Select the root folder of a git working copy";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char path[MAX_PATH];
    SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    std::string spath(path);
    if (!GitProbe::IsRepo(spath)) { m_StatusLabel->Text = "Not a git repository!"; return; }
    auto name = fs::path(spath).filename().string();
    for (auto& r : m_Repos) if (r.Path == spath) { m_StatusLabel->Text = "Already in list"; return; }
    m_Repos.push_back({name, spath, ""});
    SaveReposToDisk(); RebuildList();
    m_StatusLabel->Text = "Added: " + name;
}

inline void GitTab::OnRemoveRepo() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Repos.size()) return;
    auto removed = m_Repos[m_SelectedIndex].Name;
    m_Repos.erase(m_Repos.begin() + m_SelectedIndex);
    SaveReposToDisk(); RebuildList();
    m_StatusLabel->Text = "Removed: " + removed;
}

inline void GitTab::OnRefresh() {
    if (m_GitExe.empty()) { m_StatusLabel->Text = "git.exe not found"; return; }
    m_SummaryByPath.clear();
    for (const GitRepoEntry& r : m_Repos) {
        m_SummaryByPath[r.Path] = GitProbe::Summarize(m_GitExe, r.Name, r.Path);
    }
    RebuildList();
}

inline void GitTab::OnFetchAll() {
    if (m_GitExe.empty()) { m_StatusLabel->Text = "git.exe not found"; return; }
    if (m_Repos.empty()) { m_StatusLabel->Text = "No repos configured"; return; }

    std::string gitEsc = m_GitExe;
    for (size_t p = 0; (p = gitEsc.find('\'', p)) != std::string::npos; p += 2)
        gitEsc.insert(p, "'");

    std::string script;
    for (auto& r : m_Repos) {
        if (!std::filesystem::exists(r.Path)) continue;
        std::string escPath = r.Path;
        for (size_t p = 0; (p = escPath.find('\'', p)) != std::string::npos; p += 2)
            escPath.insert(p, "'");
        script += "Write-Host '=== " + r.Name + " ===' -ForegroundColor Cyan; ";
        script += "Set-Location -LiteralPath '" + escPath + "'; ";
        script += "& '" + gitEsc + "' fetch --all --prune; ";
    }
    if (script.empty()) { m_StatusLabel->Text = "No valid repos found"; return; }
    script += "Write-Host ''; Write-Host 'Done. Press Enter to close.' -ForegroundColor Yellow; [void](Read-Host)";

    std::string encoded;
    for (auto c : script) {
        if (c == '"') encoded += "\\\"";
        else encoded += c;
    }
    std::string args = "-NoProfile -ExecutionPolicy Bypass -NoExit -Command \"" + encoded + "\"";
    ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    m_StatusLabel->Text = "Fetch All started for " + std::to_string(m_Repos.size()) + " repo(s)";
}

inline void GitTab::OnOpenFolder() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size())
        Tools::OpenExplorer(m_Repos[m_SelectedIndex].Path);
}

inline void GitTab::OnOpenTerminal() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        std::string esc = m_Repos[m_SelectedIndex].Path;
        for (size_t p = 0; (p = esc.find('\'', p)) != std::string::npos; p += 2)
            esc.insert(p, "'");
        std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + esc + "'\"";
        ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    }
}

inline void GitTab::OnOpenIde() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        Tools::OpenInIde(m_Repos[m_SelectedIndex].Path);
        m_StatusLabel->Text = "Opening in IDE: " + m_Repos[m_SelectedIndex].Name;
    }
}

inline void GitTab::OnGitPull() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        std::string esc = m_Repos[m_SelectedIndex].Path;
        for (size_t p = 0; (p = esc.find('\'', p)) != std::string::npos; p += 2) esc.insert(p, "'");
        std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + esc + "'; git pull --ff-only; Write-Host 'Press Enter...'; Read-Host\"";
        ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    }
}

inline void GitTab::OnGitPush() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        std::string esc = m_Repos[m_SelectedIndex].Path;
        for (size_t p = 0; (p = esc.find('\'', p)) != std::string::npos; p += 2) esc.insert(p, "'");
        std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + esc + "'; git push; Write-Host 'Press Enter...'; Read-Host\"";
        ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    }
}

inline void GitTab::OnGitFetch() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        std::string esc = m_Repos[m_SelectedIndex].Path;
        for (size_t p = 0; (p = esc.find('\'', p)) != std::string::npos; p += 2) esc.insert(p, "'");
        std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + esc + "'; git fetch --all --prune; Write-Host 'Press Enter...'; Read-Host\"";
        ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    }
}

inline void GitTab::OnGitStatus() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        std::string esc = m_Repos[m_SelectedIndex].Path;
        for (size_t p = 0; (p = esc.find('\'', p)) != std::string::npos; p += 2) esc.insert(p, "'");
        std::string args = "-NoExit -NoProfile -Command \"Set-Location -LiteralPath '" + esc + "'; git status; Write-Host 'Press Enter...'; Read-Host\"";
        ShellExecuteA(nullptr, "open", "powershell.exe", args.c_str(), nullptr, SW_SHOW);
    }
}

inline void GitTab::OnUgitLog() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        if (m_UgitPath.empty()) { m_StatusLabel->Text = "UGit not found"; return; }
        system(("cmd /c " + m_UgitPath + " show-repo-log \"" + m_Repos[m_SelectedIndex].Path + "\"").c_str());
    }
}

inline void GitTab::OnUgitOpen() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        if (m_UgitPath.empty()) { m_StatusLabel->Text = "UGit not found"; return; }
        system(("cmd /c " + m_UgitPath + " open \"" + m_Repos[m_SelectedIndex].Path + "\"").c_str());
    }
}

inline void GitTab::OnCopyPath() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Repos.size()) {
        CopyTextToClipboard(m_Repos[m_SelectedIndex].Path);
        m_StatusLabel->Text = "Copied: " + m_Repos[m_SelectedIndex].Path;
    }
}

inline std::string GitTab::FormatRow(int index) const {
    if (index < 0 || index >= static_cast<int>(m_Repos.size())) return {};

    const GitRepoEntry& repo = m_Repos[index];
    const std::string tagPart = repo.Tag.empty() ? std::string() : ("[" + repo.Tag + "] ");

    char buf[512];
    const auto summary = m_SummaryByPath.find(repo.Path);
    if (summary == m_SummaryByPath.end()) {
        snprintf(buf, sizeof(buf), "%s%-14s %s",
                 tagPart.c_str(), repo.Name.c_str(), repo.Path.c_str());
    } else if (summary->second.Ok) {
        snprintf(buf, sizeof(buf), "%s%-14s %-18s +%d/-%d  %s",
                 tagPart.c_str(), repo.Name.c_str(), summary->second.Branch.c_str(),
                 summary->second.Ahead, summary->second.Behind, repo.Path.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s%-14s [!%s]  %s",
                 tagPart.c_str(), repo.Name.c_str(), summary->second.Error.c_str(),
                 repo.Path.c_str());
    }
    return buf;
}

inline ZSlate::UIColor GitTab::RowColorFor(int index) const {
    const ZSlate::UIColor fallback(0.85f, 0.88f, 0.92f, 1.0f);
    if (index < 0 || index >= static_cast<int>(m_Repos.size())) return fallback;

    const std::string tag = GitTags::Trim(m_Repos[index].Tag);
    if (tag.empty()) return fallback;

    const auto style = m_TagStyles.find(GitTags::KeyOf(tag));
    if (style == m_TagStyles.end() || style->second.ForeArgb == 0) return fallback;
    return GArgbToColor(style->second.ForeArgb);
}

inline void GitTab::RegroupByTag() {
    std::string selectedPath;
    if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Repos.size()))
        selectedPath = m_Repos[m_SelectedIndex].Path;

    std::stable_sort(m_Repos.begin(), m_Repos.end(),
        [](const GitRepoEntry& a, const GitRepoEntry& b) {
            const bool aEmpty = GitTags::Trim(a.Tag).empty();
            const bool bEmpty = GitTags::Trim(b.Tag).empty();
            if (aEmpty != bEmpty) return !aEmpty;                 // tagged first
            if (!aEmpty) {
                const int byTag = GCompareCI(GitTags::Trim(a.Tag), GitTags::Trim(b.Tag));
                if (byTag != 0) return byTag < 0;
            }
            return GCompareCI(a.Name, b.Name) < 0;
        });

    if (selectedPath.empty()) return;
    for (size_t i = 0; i < m_Repos.size(); ++i) {
        if (m_Repos[i].Path == selectedPath) {
            m_SelectedIndex = static_cast<int>(i);
            m_ListView->SetSelection(static_cast<int32_t>(i), true);
            break;
        }
    }
}

inline void GitTab::LoadTagStylesFromDisk() {
    m_TagStyles.clear();
    for (const GitTags::GitTagStyle& style : GitTags::Load()) {
        const std::string key = GitTags::KeyOf(style.Tag);
        if (!key.empty()) m_TagStyles[key] = style;
    }
}

inline void GitTab::PersistTagStylesToDisk() {
    std::vector<GitTags::GitTagStyle> styles;
    styles.reserve(m_TagStyles.size());
    for (const auto& entry : m_TagStyles) styles.push_back(entry.second);
    GitTags::Save(styles);
}

inline void GitTab::PruneOrphanTagStyles() {
    std::vector<std::string> used;
    for (const GitRepoEntry& repo : m_Repos) {
        const std::string tag = GitTags::Trim(repo.Tag);
        if (!tag.empty()) used.push_back(GitTags::KeyOf(tag));
    }
    for (auto it = m_TagStyles.begin(); it != m_TagStyles.end(); ) {
        const bool orphan = std::find(used.begin(), used.end(), it->first) == used.end();
        if (orphan && it->second.ForeArgb == 0 && it->second.BackArgb == 0)
            it = m_TagStyles.erase(it);
        else
            ++it;
    }
}

inline void GitTab::OnSetTag() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Repos.size())) return;
    ShowSetTagDialog(m_SelectedIndex);
}

inline void GitTab::ShowSetTagDialog(int index) {
    if (index < 0 || index >= static_cast<int>(m_Repos.size())) return;

    const std::string repoName   = m_Repos[index].Name;
    const std::string currentTag = GitTags::Trim(m_Repos[index].Tag);

    int fore = 0, back = 0;
    if (!currentTag.empty()) {
        const auto existing = m_TagStyles.find(GitTags::KeyOf(currentTag));
        if (existing != m_TagStyles.end()) {
            fore = existing->second.ForeArgb;
            back = existing->second.BackArgb;
        }
    }

    auto dialog = std::make_shared<ZSlate::SDialog>();
    dialog->SetTitle("Set Tag");

    auto body = std::make_shared<ZSlate::SVerticalBox>();

    auto tagLabel = std::make_shared<ZSlate::STextBlock>();
    tagLabel->Text     = "Tag:  (empty or \"(no tag)\" clears the tag)";
    tagLabel->FontSize = 13.0f;
    tagLabel->Color    = ZSlate::UIColor(0.75f, 0.78f, 0.85f, 1.0f);
    body->AddSlot(tagLabel);
    body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 3.0f)));

    auto tagBox = std::make_shared<ZSlate::SEditableTextBox>();
    tagBox->FontSize = 13.0f;
    tagBox->MinWidth = 330.0f;
    tagBox->SetText(currentTag);
    body->AddSlot(tagBox);
    body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 12.0f)));

    if (!m_TagStyles.empty()) {
        std::string known;
        int shown = 0;
        for (const auto& entry : m_TagStyles) {
            if (shown++ >= 6) break;
            if (!known.empty()) known += ", ";
            known += entry.second.Tag;
        }
        if (known.size() > 60) known = known.substr(0, 57) + "...";

        auto knownLabel = std::make_shared<ZSlate::STextBlock>();
        knownLabel->Text     = "Existing: " + known;
        knownLabel->FontSize = 12.0f;
        knownLabel->Color    = ZSlate::UIColor(0.58f, 0.61f, 0.70f, 1.0f);
        body->AddSlot(knownLabel);
        body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 10.0f)));
    }

    auto foreValue = std::make_shared<int>(fore);
    auto backValue = std::make_shared<int>(back);

    auto MakeSwatch = []() {
        auto swatch = std::make_shared<ZSlate::SBorder>();
        swatch->Padding = ZSlate::Vector2(0.0f, 0.0f);
        swatch->BorderColor = ZSlate::UIColor(0.45f, 0.48f, 0.55f, 1.0f);
        swatch->SetContent(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(52.0f, 16.0f)));
        return swatch;
    };

    auto foreSwatch = MakeSwatch();
    auto backSwatch = MakeSwatch();

    auto foreText = std::make_shared<ZSlate::STextBlock>();
    foreText->FontSize = 13.0f;
    foreText->Color    = ZSlate::UIColor(0.80f, 0.83f, 0.90f, 1.0f);
    auto backText = std::make_shared<ZSlate::STextBlock>();
    backText->FontSize = 13.0f;
    backText->Color    = ZSlate::UIColor(0.80f, 0.83f, 0.90f, 1.0f);

    auto preview      = std::make_shared<ZSlate::STextBlock>();
    preview->FontSize = 14.0f;
    preview->Text     = "  Preview: " + repoName + "  ";
    auto previewBox   = std::make_shared<ZSlate::SBorder>();
    previewBox->Padding = ZSlate::Vector2(6.0f, 6.0f);
    previewBox->SetContent(preview);

    auto Refresh = [=]() {
        foreSwatch->BackgroundColor = *foreValue ? GArgbToColor(*foreValue)
                                                 : ZSlate::UIColor(0.22f, 0.24f, 0.29f, 1.0f);
        backSwatch->BackgroundColor = *backValue ? GArgbToColor(*backValue)
                                                 : ZSlate::UIColor(0.22f, 0.24f, 0.29f, 1.0f);
        foreText->Text = GArgbToHex(*foreValue);
        backText->Text = GArgbToHex(*backValue);
        preview->Color      = *foreValue ? GArgbToColor(*foreValue)
                                         : ZSlate::UIColor(0.90f, 0.92f, 0.95f, 1.0f);
        previewBox->BackgroundColor = *backValue ? GArgbToColor(*backValue)
                                                 : ZSlate::UIColor(0.16f, 0.17f, 0.21f, 1.0f);
    };
    Refresh();

    auto MakeColorRow = [&](const char* caption,
                            const std::shared_ptr<ZSlate::SBorder>& swatch,
                            const std::shared_ptr<ZSlate::STextBlock>& text,
                            const std::shared_ptr<int>& value) {
        auto row = std::make_shared<ZSlate::SHorizontalBox>();
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text     = caption;
        label->FontSize = 13.0f;
        label->Color    = ZSlate::UIColor(0.75f, 0.78f, 0.85f, 1.0f);
        row->AddSlot(label);
        row->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));
        row->AddSlot(swatch);
        row->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));
        row->AddSlot(text);
        row->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(10.0f, 1.0f)));
        row->AddSlot(GMakeBtn("Pick...", [this, value, Refresh]() {
            if (GPickColor(GetActiveWindow(), *value)) Refresh();
        }));
        row->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(6.0f, 1.0f)));
        row->AddSlot(GMakeBtn("Default", [value, Refresh]() { *value = 0; Refresh(); }));
        return row;
    };

    body->AddSlot(MakeColorRow("Foreground:", foreSwatch, foreText, foreValue));
    body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 8.0f)));
    body->AddSlot(MakeColorRow("Background:", backSwatch, backText, backValue));
    body->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 12.0f)));
    body->AddSlot(previewBox);

    dialog->SetBody(body);

    auto buttons = std::make_shared<ZSlate::SHorizontalBox>();
    std::weak_ptr<ZSlate::SDialog> weakDialog = dialog;

    buttons->AddSlot(GMakeBtn("OK", [this, weakDialog, index, repoName, tagBox, foreValue, backValue]() {
        std::string newTag = GitTags::Trim(tagBox->Text);
        if (newTag == "(no tag)") newTag.clear();

        m_Repos[index].Tag = newTag;
        if (!newTag.empty()) {
            GitTags::GitTagStyle style;
            style.Tag      = newTag;
            style.ForeArgb = *foreValue;
            style.BackArgb = *backValue;
            m_TagStyles[GitTags::KeyOf(newTag)] = style;
        }

        PruneOrphanTagStyles();
        SaveReposToDisk();
        PersistTagStylesToDisk();
        RebuildList();

        m_StatusLabel->Text = newTag.empty() ? ("Cleared tag for: " + repoName)
                                             : ("Tagged '" + newTag + "': " + repoName);
        if (auto d = weakDialog.lock()) d->Close();
    }));
    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(10.0f, 1.0f)));
    buttons->AddSlot(GMakeBtn("Cancel", [weakDialog]() {
        if (auto d = weakDialog.lock()) d->Close();
    }));
    dialog->SetButtons(buttons);
    dialog->Show();
}

inline void GitTab::RebuildList() {
    RegroupByTag();

    std::vector<std::string> disp;
    disp.reserve(m_Repos.size());
    for (int i = 0; i < static_cast<int>(m_Repos.size()); ++i) disp.push_back(FormatRow(i));
    if (disp.empty()) disp.push_back("No repos. Click 'Add Repo...' to add one.");

    m_DataSource->SetItems(disp);
    m_StatusLabel->Text = std::to_string(m_Repos.size()) + " repo(s)";
}

inline void GitTab::Refresh() { OnRefresh(); }

inline void GitTab::OnRepoRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_Repos.size()) return;

    m_SelectedIndex = index;
    m_ListView->SetSelection(index, true);

    HWND hWnd = GetActiveWindow();
    std::vector<std::pair<std::string, int>> items = {
        {"Open Folder",        1},
        {"Open Terminal Here", 2},
        {"Open in IDE",        3},
        {"-",                  0},
        {"Git Fetch",          4},
        {"Git Pull",           5},
        {"Git Push",           6},
        {"Git Status",         7},
        {"-",                  0},
        {"UGit Log",           8},
        {"Open in UGit",       9},
        {"-",                  0},
        {"Copy Path",          10},
        {"Refresh",            12},
        {"-",                  0},
        {"Set Tag...",         13},
        {"-",                  0},
        {"Remove From List",   11},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);

    switch (cmd) {
    case 1:  OnOpenFolder();  break;
    case 2:  OnOpenTerminal(); break;
    case 3:  OnOpenIde();     break;
    case 4:  OnGitFetch();    break;
    case 5:  OnGitPull();     break;
    case 6:  OnGitPush();     break;
    case 7:  OnGitStatus();   break;
    case 8:  OnUgitLog();     break;
    case 9:  OnUgitOpen();    break;
    case 10: OnCopyPath();    break;
    case 11: OnRemoveRepo();  break;
    case 12: OnRefresh();     break;
    case 13: OnSetTag();      break;
    }
}

}
