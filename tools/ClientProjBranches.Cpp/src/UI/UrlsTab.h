#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "ContextMenuHelper.h"
#include "FieldDialog.h"
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <shellapi.h>

namespace ClientProj {

struct UrlEntry { std::string Title, Url; };

class UrlsTab {
public:
    UrlsTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
private:
    void BuildUI(); void LoadFromDisk(); void SaveToDisk();
    void OnAdd(); void OnEdit(); void OnRemove(); void OnOpen(); void OnCopy();
    void OnUrlRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void Move(int delta); void RebuildList();

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::SListView<std::string>> m_ListView;
    std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_DataSource;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;
    std::vector<UrlEntry> m_Entries;
    int m_SelectedIndex = -1;
};

inline std::shared_ptr<ZSlate::SButton> UMkBtn(const std::string& t, std::function<void()> cb) {
    auto b = std::make_shared<ZSlate::SButton>();
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = 13.0f; l->Color = ZSlate::Colors::White;
    b->SetContent(l); b->OnClicked = std::move(cb); return b;
}

inline UrlsTab::UrlsTab() : m_Root(std::make_shared<ZSlate::SVerticalBox>()) {
    m_DataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
    m_ListView = std::make_shared<ZSlate::SListView<std::string>>();
    m_ListView->SetDataSource(m_DataSource);
    m_ListView->Options.ItemHeight = 26.0f;
    m_ListView->SetOnGenerateItemWidget([this](const std::string& item, int32_t, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 13.0f;
        w->Color = ZSlate::UIColor(0.85f, 0.88f, 0.92f, 1.0f);
        return w;
    });
    m_ListView->SetOnSelectionChanged([this](const auto& sel) { m_SelectedIndex = sel.empty() ? -1 : sel[0]; });
    // C# UrlsTab.CellDoubleClick opens the entry.
    m_ListView->SetOnItemDoubleClicked([this](int32_t) { OnOpen(); });
    m_ListView->SetOnItemRightClicked([this](int32_t index, const ZSlate::Vector2& pos) {
        OnUrlRightClick(index, pos);
    });
    BuildUI(); LoadFromDisk(); RebuildList();
}

inline void UrlsTab::BuildUI() {
    auto tb = std::make_shared<ZSlate::SHorizontalBox>();
    tb->AddSlot(UMkBtn("Add...",     [this]() { OnAdd(); }));
    tb->AddSlot(UMkBtn("Edit...",    [this]() { OnEdit(); }));
    tb->AddSlot(UMkBtn("Remove",     [this]() { OnRemove(); }));
    tb->AddSlot(UMkBtn("Move Up",    [this]() { Move(-1); }));
    tb->AddSlot(UMkBtn("Move Down",  [this]() { Move(1); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    tb->AddSlot(UMkBtn("Open",       [this]() { OnOpen(); }));
    tb->AddSlot(UMkBtn("Copy URL",   [this]() { OnCopy(); }));
    m_Root->AddSlot(tb);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_ListView);
    m_StatusLabel = std::make_shared<ZSlate::STextBlock>();
    m_StatusLabel->Text = "Ready"; m_StatusLabel->FontSize = 13.0f;
    m_StatusLabel->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_StatusLabel);
}

inline void UrlsTab::LoadFromDisk() {
    m_Entries.clear();

    // C# writes a bare array of {"Title","Url"} objects.
    for (const auto& value : Json::LoadArrayFromFile(Paths::Urls())) {
        if (!value) continue;
        if (value->IsString()) {
            UrlEntry entry; entry.Url = value->AsString();
            m_Entries.push_back(entry);
            continue;
        }
        if (!value->IsObject()) continue;
        const Json::Object& object = value->AsObject();
        UrlEntry entry;
        entry.Title = Json::OptStringCI(object, "Title");
        entry.Url   = Json::OptStringCI(object, "Url");
        if (!entry.Url.empty()) m_Entries.push_back(entry);
    }

    if (!m_Entries.empty()) return;

    // Legacy C++ shape: {"urls":[{"title","url"}]}
    const Json::Object legacy = Json::LoadFromFile(Paths::Urls());
    if (const Json::Array* array = Json::OptArray(legacy, "urls")) {
        for (const auto& value : *array) {
            if (!value || !value->IsObject()) continue;
            const Json::Object& object = value->AsObject();
            UrlEntry entry;
            entry.Title = Json::OptStringCI(object, "Title");
            entry.Url   = Json::OptStringCI(object, "Url");
            if (!entry.Url.empty()) m_Entries.push_back(entry);
        }
    }
}

inline void UrlsTab::SaveToDisk() {
    // Bare array, PascalCase keys — identical to C# UrlStore.
    Json::Array array;
    array.reserve(m_Entries.size());
    for (const UrlEntry& entry : m_Entries) {
        Json::Object object;
        object["Title"] = std::make_shared<Json::Value>(entry.Title);
        object["Url"]   = std::make_shared<Json::Value>(entry.Url);
        array.push_back(std::make_shared<Json::Value>(object));
    }
    Json::SaveArrayToFile(Paths::Urls(), array);
}

inline void UrlsTab::OnAdd() {
    UrlEntry e; e.Url = "https://";
    // Try to paste from clipboard if it contains a URL
    if (IsClipboardFormatAvailable(CF_TEXT) && OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_TEXT);
        if (h) {
            char* t = (char*)GlobalLock(h);
            if (t && (strstr(t, "http://") || strstr(t, "https://"))) {
                e.Url = t;
                e.Title = "New Bookmark";
            }
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    m_Entries.push_back(e);
    m_SelectedIndex = (int)m_Entries.size() - 1;
    m_ListView->SetSelection(m_SelectedIndex, true);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Added.";
    OnEdit();   // let the user fill in Title / URL
}

inline void UrlsTab::OnEdit() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Entries.size()) return;

    const UrlEntry entry = m_Entries[m_SelectedIndex];

    ShowFieldsDialog("URL", {
        { "Title", entry.Title, false, 380.0f, 0.0f },
        { "URL",   entry.Url,   false, 380.0f, 0.0f },
    }, [this](const std::vector<std::string>& values) {
        if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Entries.size()) return;
        m_Entries[m_SelectedIndex].Title = values.size() > 0 ? values[0] : "";
        m_Entries[m_SelectedIndex].Url   = values.size() > 1 ? values[1] : "";
        SaveToDisk();
        RebuildList();
        m_StatusLabel->Text = "Updated";
    });
}

inline void UrlsTab::OnRemove() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Entries.size()) return;
    m_Entries.erase(m_Entries.begin() + m_SelectedIndex);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Removed";
}

inline void UrlsTab::OnOpen() {
    if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Entries.size())
        ShellExecuteA(nullptr, "open", m_Entries[m_SelectedIndex].Url.c_str(), nullptr, nullptr, SW_SHOW);
}

inline void UrlsTab::OnCopy() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Entries.size()) return;
    auto& u = m_Entries[m_SelectedIndex].Url;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, u.size() + 1);
        if (h) { memcpy(GlobalLock(h), u.c_str(), u.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
        CloseClipboard();
    }
    m_StatusLabel->Text = "Copied";
}

inline void UrlsTab::Move(int delta) {
    if (m_SelectedIndex < 0) return;
    int ni = m_SelectedIndex + delta;
    if (ni < 0 || ni >= (int)m_Entries.size()) return;
    std::swap(m_Entries[m_SelectedIndex], m_Entries[ni]);
    m_SelectedIndex = ni;
    SaveToDisk(); RebuildList();
}

inline void UrlsTab::RebuildList() {
    std::vector<std::string> disp;
    for (auto& e : m_Entries) {
        char buf[512]; snprintf(buf, sizeof(buf), "%-20s %s", e.Title.c_str(), e.Url.c_str());
        disp.push_back(buf);
    }
    if (disp.empty()) disp.push_back("No URLs. Click 'Add...' to add (pastes from clipboard).");
    m_DataSource->SetItems(disp);
    m_StatusLabel->Text = std::to_string(m_Entries.size()) + " URL(s)";
}

inline void UrlsTab::OnUrlRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_Entries.size()) return;

    m_SelectedIndex = index;
    m_ListView->SetSelection(index, true);

    HWND hWnd = GetActiveWindow();
    std::vector<std::pair<std::string, int>> items = {
        {"Open",      1},
        {"Copy URL",  2},
        {"-",         0},
        {"Add...",    3},
        {"Edit...",   4},
        {"Remove",    5},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);

    switch (cmd) {
    case 1: OnOpen();   break;
    case 2: OnCopy();   break;
    case 3: OnAdd();    break;
    case 4: OnEdit();   break;
    case 5: OnRemove(); break;
    }
}

}
