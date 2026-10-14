#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "ContextMenuHelper.h"
#include "FieldDialog.h"
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>
#include <Windows.h>
#include <shellapi.h>

namespace ClientProj {

struct TipEntry { std::string Title, Url, Tags, Text, Created, Updated; };

class TipsTab {
public:
    TipsTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
    void Reload();
private:
    void BuildUI(); void SaveToDisk();
    void OnAdd(); void OnEdit(); void OnRemove(); void OnOpenUrl();
    void OnCopyTitle(); void OnCopyBody(); void OnCopyUrl();
    void OnFilterChanged(const std::string& text);
    void OnTipRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void RebuildList();

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::SListView<std::string>> m_ListView;
    std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_DataSource;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel, m_PreviewLabel;
    std::shared_ptr<ZSlate::SEditableTextBox> m_FilterBox;
    std::vector<TipEntry> m_Tips;
    std::string m_FilterText;
    int m_SelectedIndex = -1;
};

inline std::shared_ptr<ZSlate::SButton> TMkBtn(const std::string& t, std::function<void()> cb) {
    auto b = std::make_shared<ZSlate::SButton>();
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = 13.0f; l->Color = ZSlate::Colors::White;
    b->SetContent(l); b->OnClicked = std::move(cb); return b;
}

inline TipsTab::TipsTab() : m_Root(std::make_shared<ZSlate::SVerticalBox>()) {
    m_DataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
    m_ListView = std::make_shared<ZSlate::SListView<std::string>>();
    m_ListView->SetDataSource(m_DataSource);
    m_ListView->ItemHeight = 26.0f;
    m_ListView->SetOnGenerateItemWidget([this](const std::string& item, int32_t, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 13.0f;
        w->Color = ZSlate::UIColor(0.85f, 0.88f, 0.92f, 1.0f);
        return w;
    });
    // C# TipsTab.CellDoubleClick opens the editor (except on the Url column).
    m_ListView->SetOnItemDoubleClicked([this](int32_t) { OnEdit(); });

    m_ListView->SetOnSelectionChanged([this](const auto& sel) {
        m_SelectedIndex = sel.empty() ? -1 : sel[0];
        if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Tips.size()) {
            auto& t = m_Tips[m_SelectedIndex];
            m_PreviewLabel->Text = "Title: " + t.Title + "\nURL: " + t.Url + "\nTags: " + t.Tags + "\n\n" + t.Text;
        } else m_PreviewLabel->Text = "";
    });
    m_ListView->SetOnItemRightClicked([this](int32_t index, const ZSlate::Vector2& pos) {
        OnTipRightClick(index, pos);
    });
    BuildUI(); Reload();
}

inline void TipsTab::BuildUI() {
    // ---- Top bar: Filter ----
    auto topBar = std::make_shared<ZSlate::SHorizontalBox>();
    auto filterLabel = std::make_shared<ZSlate::STextBlock>();
    filterLabel->Text = "Filter:"; filterLabel->FontSize = 14.0f;
    filterLabel->Color = ZSlate::UIColor(0.6f, 0.65f, 0.75f, 1.0f);
    topBar->AddSlot(filterLabel);

    m_FilterBox = std::make_shared<ZSlate::SEditableTextBox>();
    m_FilterBox->MinWidth = 260.0f;
    m_FilterBox->FontSize = 14.0f;
    m_FilterBox->OnTextChanged = [this](const std::string& text) { OnFilterChanged(text); };
    topBar->AddSlot(m_FilterBox);
    topBar->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    m_Root->AddSlot(topBar);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    // ---- Toolbar ----
    auto tb = std::make_shared<ZSlate::SHorizontalBox>();
    tb->AddSlot(TMkBtn("Add...",   [this]() { OnAdd(); }));
    tb->AddSlot(TMkBtn("Edit...",  [this]() { OnEdit(); }));
    tb->AddSlot(TMkBtn("Remove",   [this]() { OnRemove(); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    tb->AddSlot(TMkBtn("Open URL", [this]() { OnOpenUrl(); }));
    m_Root->AddSlot(tb);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    // ---- List ----
    m_Root->AddSlot(m_ListView).Fill();

    // ---- Preview ----
    m_PreviewLabel = std::make_shared<ZSlate::STextBlock>();
    m_PreviewLabel->Text = ""; m_PreviewLabel->FontSize = 13.0f;
    m_PreviewLabel->Color = ZSlate::UIColor(0.6f, 0.65f, 0.75f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_PreviewLabel);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));

    // ---- Status ----
    m_StatusLabel = std::make_shared<ZSlate::STextBlock>();
    m_StatusLabel->Text = "Ready"; m_StatusLabel->FontSize = 13.0f;
    m_StatusLabel->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(m_StatusLabel);
}

inline void TipsTab::Reload() {
    m_Tips.clear();

    // C# writes a bare array of {"Title","Url","Tags","Text","Created","Updated"}.
    for (const auto& value : Json::LoadArrayFromFile(Paths::Tips())) {
        if (!value || !value->IsObject()) continue;
        const Json::Object& object = value->AsObject();
        TipEntry tip;
        tip.Title   = Json::OptStringCI(object, "Title");
        tip.Url     = Json::OptStringCI(object, "Url");
        tip.Tags    = Json::OptStringCI(object, "Tags");
        tip.Text    = Json::OptStringCI(object, "Text");
        tip.Created = Json::OptStringCI(object, "Created");
        tip.Updated = Json::OptStringCI(object, "Updated");
        m_Tips.push_back(tip);
    }

    if (!m_Tips.empty()) { RebuildList(); return; }

    // Legacy C++ shape: {"tips":[{...lowercase...}]}
    const Json::Object legacy = Json::LoadFromFile(Paths::Tips());
    if (const Json::Array* array = Json::OptArray(legacy, "tips")) {
        for (const auto& value : *array) {
            if (!value || !value->IsObject()) continue;
            const Json::Object& object = value->AsObject();
            TipEntry tip;
            tip.Title   = Json::OptStringCI(object, "Title");
            tip.Url     = Json::OptStringCI(object, "Url");
            tip.Tags    = Json::OptStringCI(object, "Tags");
            tip.Text    = Json::OptStringCI(object, "Text");
            tip.Created = Json::OptStringCI(object, "Created");
            tip.Updated = Json::OptStringCI(object, "Updated");
            m_Tips.push_back(tip);
        }
    }
    RebuildList();
}

inline void TipsTab::SaveToDisk() {
    // Bare array, PascalCase keys — identical to C# TipStore.
    Json::Array array;
    array.reserve(m_Tips.size());
    for (const TipEntry& tip : m_Tips) {
        Json::Object object;
        object["Title"]   = std::make_shared<Json::Value>(tip.Title);
        object["Url"]     = std::make_shared<Json::Value>(tip.Url);
        object["Tags"]    = std::make_shared<Json::Value>(tip.Tags);
        object["Text"]    = std::make_shared<Json::Value>(tip.Text);
        object["Created"] = std::make_shared<Json::Value>(tip.Created);
        object["Updated"] = std::make_shared<Json::Value>(tip.Updated);
        array.push_back(std::make_shared<Json::Value>(object));
    }
    Json::SaveArrayToFile(Paths::Tips(), array);
}

inline void TipsTab::OnFilterChanged(const std::string& text) {
    m_FilterText = text;
    RebuildList();
}

inline void TipsTab::OnAdd() {
    TipEntry t;
    auto tm = time(nullptr); char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&tm));
    t.Created = t.Updated = buf;
    m_Tips.push_back(t);
    m_SelectedIndex = (int)m_Tips.size() - 1;
    m_ListView->SetSelection(m_SelectedIndex, true);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Added.";
    OnEdit();
}

inline void TipsTab::OnEdit() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;

    const TipEntry tip = m_Tips[m_SelectedIndex];

    ShowFieldsDialog("Tip", {
        { "Title", tip.Title, false, 380.0f, 0.0f },
        { "Tags (comma separated)", tip.Tags, false, 380.0f, 0.0f },
        { "URL", tip.Url, false, 380.0f, 0.0f },
        { "Text", tip.Text, true, 380.0f, 160.0f },
    }, [this](const std::vector<std::string>& values) {
        if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;

        auto tm = time(nullptr); char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&tm));

        TipEntry& tip = m_Tips[m_SelectedIndex];
        tip.Title   = values.size() > 0 ? values[0] : "";
        tip.Tags    = values.size() > 1 ? values[1] : "";
        tip.Url     = values.size() > 2 ? values[2] : "";
        tip.Text    = values.size() > 3 ? values[3] : "";
        tip.Updated = buf;
        if (tip.Created.empty()) tip.Created = buf;

        SaveToDisk(); RebuildList();
        m_StatusLabel->Text = "Updated";
    });
}

inline void TipsTab::OnRemove() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;
    m_Tips.erase(m_Tips.begin() + m_SelectedIndex);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Removed";
}

inline void TipsTab::OnOpenUrl() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;
    auto& u = m_Tips[m_SelectedIndex].Url;
    if (u.empty()) { m_StatusLabel->Text = "No URL"; return; }
    ShellExecuteA(nullptr, "open", u.c_str(), nullptr, nullptr, SW_SHOW);
    m_StatusLabel->Text = "Opened: " + u;
}

inline void TipsTab::OnCopyTitle() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;
    auto& text = m_Tips[m_SelectedIndex].Title;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
    m_StatusLabel->Text = "Copied title";
}

inline void TipsTab::OnCopyBody() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;
    auto& text = m_Tips[m_SelectedIndex].Text;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
    m_StatusLabel->Text = "Copied body";
}

inline void TipsTab::OnCopyUrl() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Tips.size()) return;
    auto& url = m_Tips[m_SelectedIndex].Url;
    if (url.empty()) { m_StatusLabel->Text = "No URL"; return; }
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, url.size() + 1);
        if (hMem) {
            memcpy(GlobalLock(hMem), url.c_str(), url.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
    m_StatusLabel->Text = "Copied URL";
}

inline void TipsTab::RebuildList() {
    // Filter by text
    std::string needle = m_FilterText;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);

    std::vector<TipEntry*> filtered;
    for (auto& t : m_Tips) {
        if (!needle.empty()) {
            std::string hay = t.Title + " " + t.Tags + " " + t.Text + " " + t.Url;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        filtered.push_back(&t);
    }
    // Sort: newest first by Updated (fallback Created)
    std::sort(filtered.begin(), filtered.end(), [](TipEntry* a, TipEntry* b) {
        std::string da = a->Updated.empty() ? a->Created : a->Updated;
        std::string db = b->Updated.empty() ? b->Created : b->Updated;
        return da > db;
    });

    std::vector<std::string> disp;
    for (auto* t : filtered) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%-30s %s [%s]", t->Title.c_str(), t->Url.c_str(), t->Tags.c_str());
        disp.push_back(buf);
    }
    if (disp.empty()) disp.push_back("No tips. Click 'Add...' to create one.");
    m_DataSource->SetItems(disp);
    m_StatusLabel->Text = std::to_string(filtered.size()) + " tip(s)"
                        + (m_FilterText.empty() ? "" : " (filtered)");
}

inline void TipsTab::OnTipRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_Tips.size()) return;

    m_SelectedIndex = index;
    m_ListView->SetSelection(index, true);

    HWND hWnd = GetActiveWindow();
    std::vector<std::pair<std::string, int>> items = {
        {"Edit...",     1},
        {"Open URL",    2},
        {"Copy URL",    3},
        {"Copy Title",  4},
        {"Copy Body",   5},
        {"-",           0},
        {"Add...",      6},
        {"Remove",      7},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);

    switch (cmd) {
    case 1: OnEdit();      break;
    case 2: OnOpenUrl();   break;
    case 3: OnCopyUrl();   break;
    case 4: OnCopyTitle(); break;
    case 5: OnCopyBody();  break;
    case 6: OnAdd();       break;
    case 7: OnRemove();    break;
    }
}

}
