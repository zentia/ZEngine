#pragma once
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Services/Paths.h"
#include "../Services/JsonConfig.h"
#include "../Services/NoteArchiveStore.h"
#include "ContextMenuHelper.h"
#include "FieldDialog.h"
#include "ZSlate/Widgets/Dialogs/SDialog.h"
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <fstream>

namespace ClientProj {

struct NoteEntry {
    bool Done = false;
    std::string Priority = "Normal", Url, Text, Created, CompletedAt;
};

class NotesTab {
public:
    NotesTab();
    std::shared_ptr<ZSlate::SWidget> GetRoot() const { return m_Root; }
    void Reload();
private:
    void BuildUI(); void SaveToDisk();
    void OnAdd(); void OnEdit(); void OnRemove(); void OnToggleDone(); void OnClearDone();
    void OnMoveUp(); void OnMoveDown(); void OnArchiveDone(); void OnSortByPriority();
    void OnOpenArchive();
    int  AutoArchiveIfDone();
    void OnOpenUrl(); void OnCopyText(); void OnCopyUrl();
    void OnNoteRightClick(int32_t index, const ZSlate::Vector2& screenPos);
    void RebuildList();

    std::shared_ptr<ZSlate::SVerticalBox> m_Root;
    std::shared_ptr<ZSlate::SListView<std::string>> m_ListView;
    std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_DataSource;
    std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;
    std::vector<NoteEntry> m_Notes;
    int  m_SelectedIndex = -1;
    bool m_FirstLoad     = true;
};

inline std::shared_ptr<ZSlate::SButton> NMkBtn(const std::string& t, std::function<void()> cb) {
    auto b = std::make_shared<ZSlate::SButton>();
    auto l = std::make_shared<ZSlate::STextBlock>();
    l->Text = t; l->FontSize = 13.0f; l->Color = ZSlate::Colors::White;
    b->SetContent(l); b->OnClicked = std::move(cb);
    return b;
}

inline NotesTab::NotesTab() : m_Root(std::make_shared<ZSlate::SVerticalBox>()) {
    m_DataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
    m_ListView = std::make_shared<ZSlate::SListView<std::string>>();
    m_ListView->SetDataSource(m_DataSource);
    m_ListView->Options.ItemHeight = 28.0f;
    m_ListView->SetOnGenerateItemWidget([this](const std::string& item, int32_t, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 13.0f;
        w->Color = ZSlate::UIColor(0.85f, 0.88f, 0.92f, 1.0f);
        return w;
    });
    m_ListView->SetOnSelectionChanged([this](const auto& sel) { m_SelectedIndex = sel.empty() ? -1 : sel[0]; });
    // C# NotesTab.CellDoubleClick opens the editor.
    m_ListView->SetOnItemDoubleClicked([this](int32_t) { OnEdit(); });
    m_ListView->SetOnItemRightClicked([this](int32_t index, const ZSlate::Vector2& pos) {
        OnNoteRightClick(index, pos);
    });
    BuildUI(); Reload();

    // Port of C# NotesTab.Load: archive everything that was completed in an
    // earlier ISO week the first time the tab comes up.
    if (m_FirstLoad) {
        m_FirstLoad = false;
        if (const int moved = AutoArchiveIfDone()) {
            SaveToDisk(); RebuildList();
            m_StatusLabel->Text = "Auto-archived " + std::to_string(moved)
                                + " note(s) completed in earlier weeks";
        }
    }
}

inline void NotesTab::BuildUI() {
    auto tb = std::make_shared<ZSlate::SHorizontalBox>();
    tb->AddSlot(NMkBtn("Add...",      [this]() { OnAdd(); }));
    tb->AddSlot(NMkBtn("Edit...",     [this]() { OnEdit(); }));
    tb->AddSlot(NMkBtn("Remove",      [this]() { OnRemove(); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8, 1)));
    tb->AddSlot(NMkBtn("Move Up",     [this]() { OnMoveUp(); }));
    tb->AddSlot(NMkBtn("Move Down",   [this]() { OnMoveDown(); }));
    tb->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(16, 1)));
    tb->AddSlot(NMkBtn("Toggle Done", [this]() { OnToggleDone(); }));
    tb->AddSlot(NMkBtn("Clear Done",  [this]() { OnClearDone(); }));
    tb->AddSlot(NMkBtn("Archive Done",[this]() { OnArchiveDone(); }));
    tb->AddSlot(NMkBtn("Sort by Priority", [this]() { OnSortByPriority(); }));
    tb->AddSlot(NMkBtn("Archive...",   [this]() { OnOpenArchive(); }));
    m_Root->AddSlot(tb);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_ListView);
    m_StatusLabel = std::make_shared<ZSlate::STextBlock>();
    m_StatusLabel->Text = "Ready"; m_StatusLabel->FontSize = 13.0f;
    m_StatusLabel->Color = ZSlate::UIColor(0.5f, 0.55f, 0.65f, 1.0f);
    m_Root->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 4)));
    m_Root->AddSlot(m_StatusLabel);
}

inline void NotesTab::Reload() {
    m_Notes.clear();

    // C# writes a bare array of {"Done","Priority","Url","Text","Created","CompletedAt"}.
    for (const auto& value : Json::LoadArrayFromFile(Paths::Notes())) {
        if (!value || !value->IsObject()) continue;
        const Json::Object& object = value->AsObject();
        NoteEntry note;
        note.Done        = Json::OptBoolCI(object, "Done");
        note.Priority    = Json::OptStringCI(object, "Priority", "Normal");
        note.Url         = Json::OptStringCI(object, "Url");
        note.Text        = Json::OptStringCI(object, "Text");
        note.Created     = Json::OptStringCI(object, "Created");
        note.CompletedAt = Json::OptStringCI(object, "CompletedAt");
        m_Notes.push_back(note);
    }

    if (!m_Notes.empty()) { RebuildList(); return; }

    // Legacy C++ shape: {"notes":[{...lowercase...}]}
    const Json::Object legacy = Json::LoadFromFile(Paths::Notes());
    if (const Json::Array* array = Json::OptArray(legacy, "notes")) {
        for (const auto& value : *array) {
            if (!value || !value->IsObject()) continue;
            const Json::Object& object = value->AsObject();
            NoteEntry note;
            note.Done        = Json::OptBoolCI(object, "Done");
            note.Priority    = Json::OptStringCI(object, "Priority", "Normal");
            note.Url         = Json::OptStringCI(object, "Url");
            note.Text        = Json::OptStringCI(object, "Text");
            note.Created     = Json::OptStringCI(object, "Created");
            note.CompletedAt = Json::OptStringCI(object, "CompletedAt");
            m_Notes.push_back(note);
        }
    }
    RebuildList();
}

inline void NotesTab::SaveToDisk() {
    // Bare array, PascalCase keys — identical to C# NoteStore.
    Json::Array array;
    array.reserve(m_Notes.size());
    for (const NoteEntry& note : m_Notes) {
        Json::Object object;
        object["Done"]        = std::make_shared<Json::Value>(note.Done);
        object["Priority"]    = std::make_shared<Json::Value>(note.Priority);
        object["Url"]         = std::make_shared<Json::Value>(note.Url);
        object["Text"]        = std::make_shared<Json::Value>(note.Text);
        object["Created"]     = std::make_shared<Json::Value>(note.Created);
        object["CompletedAt"] = std::make_shared<Json::Value>(note.CompletedAt);
        array.push_back(std::make_shared<Json::Value>(object));
    }
    Json::SaveArrayToFile(Paths::Notes(), array);
}

inline void NotesTab::OnAdd() {
    NoteEntry n; n.Priority = "Normal";
    auto t = time(nullptr); char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    n.Created = buf;
    m_Notes.push_back(n);
    m_SelectedIndex = (int)m_Notes.size() - 1;
    m_ListView->SetSelection(m_SelectedIndex, true);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Added note.";
    OnEdit();
}

inline void NotesTab::OnEdit() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;

    const NoteEntry note = m_Notes[m_SelectedIndex];

    ShowFieldsDialog("Note", {
        { "Text", note.Text, true, 380.0f, 160.0f },
        { "Priority (High / Normal / Low)", note.Priority, false, 380.0f, 0.0f },
        { "URL", note.Url, false, 380.0f, 0.0f },
    }, [this](const std::vector<std::string>& values) {
        if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;

        NoteEntry& note = m_Notes[m_SelectedIndex];
        note.Text     = values.size() > 0 ? values[0] : "";
        note.Priority = values.size() > 1 ? values[1] : "Normal";
        note.Url      = values.size() > 2 ? values[2] : "";
        if (note.Priority.empty()) note.Priority = "Normal";

        SaveToDisk(); RebuildList();
        m_StatusLabel->Text = "Updated";
    });
}

inline void NotesTab::OnRemove() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    m_Notes.erase(m_Notes.begin() + m_SelectedIndex);
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Removed";
}

inline void NotesTab::OnToggleDone() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    auto& n = m_Notes[m_SelectedIndex]; n.Done = !n.Done;
    if (n.Done) { auto t = time(nullptr); char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", localtime(&t)); n.CompletedAt = b; }
    else n.CompletedAt.clear();
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = n.Done ? "Marked done" : "Marked not done";
}

inline void NotesTab::OnMoveUp() {
    if (m_SelectedIndex <= 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    std::swap(m_Notes[m_SelectedIndex], m_Notes[m_SelectedIndex - 1]);
    m_SelectedIndex--;
    SaveToDisk(); RebuildList();
}

inline void NotesTab::OnMoveDown() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size() - 1) return;
    std::swap(m_Notes[m_SelectedIndex], m_Notes[m_SelectedIndex + 1]);
    m_SelectedIndex++;
    SaveToDisk(); RebuildList();
}

inline void NotesTab::OnClearDone() {
    auto it = std::remove_if(m_Notes.begin(), m_Notes.end(), [](auto& n){ return n.Done; });
    int c = (int)(m_Notes.end() - it); m_Notes.erase(it, m_Notes.end());
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = std::to_string(c) + " done note(s) cleared";
}

// Port of C# NotesTab.AutoArchiveIfDone: only notes completed in an *earlier*
// ISO week are moved; notes completed this week stay visible.
inline int NotesTab::AutoArchiveIfDone() {
    const std::string thisWeek = NoteArchive::IsoWeekTag(std::time(nullptr));

    std::vector<NoteEntry> survivors;
    std::vector<NoteArchive::NoteArchiveEntry> moved;

    for (const NoteEntry& note : m_Notes) {
        if (!note.Done) { survivors.push_back(note); continue; }

        std::string completed = note.CompletedAt;
        if (completed.empty()) completed = NoteArchive::NowString();

        const std::string tag = NoteArchive::WeekTagOf(completed);
        if (tag == thisWeek) { survivors.push_back(note); continue; }

        NoteArchive::NoteArchiveEntry entry;
        entry.Priority    = NoteArchive::NormalizePriority(note.Priority);
        entry.Text        = note.Text;
        entry.Url         = note.Url;
        entry.Created     = note.Created;
        entry.CompletedAt = completed;
        entry.WeekTag     = tag;
        moved.push_back(entry);
    }

    if (moved.empty()) return 0;

    std::vector<NoteArchive::NoteArchiveEntry> archive = NoteArchive::Load();
    archive.insert(archive.end(), moved.begin(), moved.end());
    NoteArchive::Save(archive);

    m_Notes = survivors;
    return static_cast<int>(moved.size());
}

inline void NotesTab::OnSortByPriority() {
    static const int rank[] = { 0, 1, 2 };   // High, Normal, Low
    auto Rank = [](const std::string& priority) {
        const std::string p = NoteArchive::NormalizePriority(priority);
        if (p == "High") return rank[0];
        if (p == "Low")  return rank[2];
        return rank[1];
    };
    std::stable_sort(m_Notes.begin(), m_Notes.end(),
        [&](const NoteEntry& a, const NoteEntry& b) { return Rank(a.Priority) < Rank(b.Priority); });
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Sorted by priority";
}

inline void NotesTab::OnArchiveDone() {
    if (m_Notes.empty()) { m_StatusLabel->Text = "No notes"; return; }

    const std::string now = NoteArchive::NowString();
    std::vector<NoteEntry> survivors;
    std::vector<NoteArchive::NoteArchiveEntry> moved;

    for (const NoteEntry& note : m_Notes) {
        if (!note.Done) { survivors.push_back(note); continue; }

        const std::string completed = note.CompletedAt.empty() ? now : note.CompletedAt;
        NoteArchive::NoteArchiveEntry entry;
        entry.Priority    = NoteArchive::NormalizePriority(note.Priority);
        entry.Text        = note.Text;
        entry.Url         = note.Url;
        entry.Created     = note.Created;
        entry.CompletedAt = completed;
        entry.WeekTag     = NoteArchive::WeekTagOf(completed);
        moved.push_back(entry);
    }

    if (moved.empty()) { m_StatusLabel->Text = "No completed notes to archive"; return; }

    // Load -> append -> save, with a rollback if the write fails.
    std::vector<NoteArchive::NoteArchiveEntry> archive = NoteArchive::Load();
    archive.insert(archive.end(), moved.begin(), moved.end());
    NoteArchive::Save(archive);

    if (NoteArchive::Load().size() < archive.size()) {
        m_StatusLabel->Text = "Archive write failed — notes kept";
        return;
    }

    m_Notes = survivors;
    SaveToDisk(); RebuildList();
    m_StatusLabel->Text = "Archived " + std::to_string(moved.size()) + " done note(s)";
}

inline void NotesTab::OnOpenArchive() {
    auto entries = std::make_shared<std::vector<NoteArchive::NoteArchiveEntry>>(NoteArchive::Load());
    if (entries->empty()) { m_StatusLabel->Text = "Archive is empty"; return; }

    // Newest week first.
    std::stable_sort(entries->begin(), entries->end(),
        [](const NoteArchive::NoteArchiveEntry& a, const NoteArchive::NoteArchiveEntry& b) {
            if (a.WeekTag != b.WeekTag) return a.WeekTag > b.WeekTag;
            return a.CompletedAt > b.CompletedAt;
        });

    auto rows = std::make_shared<std::vector<std::string>>();
    for (const NoteArchive::NoteArchiveEntry& entry : *entries) {
        std::string firstLine = entry.Text;
        const size_t newline = firstLine.find('\n');
        if (newline != std::string::npos) firstLine = firstLine.substr(0, newline);
        while (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
        if (firstLine.size() > 60) firstLine = firstLine.substr(0, 57) + "...";

        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s  [%s] %s  %s",
                      entry.WeekTag.c_str(), entry.Priority.c_str(),
                      entry.CompletedAt.c_str(), firstLine.c_str());
        rows->push_back(buf);
    }

    auto dialog = std::make_shared<ZSlate::SDialog>();
    dialog->SetTitle("Notes Archive");

    auto dataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(*rows);
    auto listView   = std::make_shared<ZSlate::SListView<std::string>>();
    listView->SetDataSource(dataSource);
    listView->Options.ItemHeight = 22.0f;
    listView->SetOnGenerateItemWidget([](const std::string& item, int32_t, const auto&) {
        auto w = std::make_shared<ZSlate::STextBlock>();
        w->Text = item; w->FontSize = 12.0f;
        w->Color = ZSlate::UIColor(0.82f, 0.85f, 0.90f, 1.0f);
        return w;
    });

    auto selected = std::make_shared<int>(-1);
    listView->SetOnSelectionChanged([selected](const auto& selection) {
        *selected = selection.empty() ? -1 : selection[0];
    });

    auto body = std::make_shared<ZSlate::SVerticalBox>();
    body->AddSlot(listView);
    dialog->SetBody(body);

    auto buttons = std::make_shared<ZSlate::SHorizontalBox>();
    std::weak_ptr<ZSlate::SDialog> weakDialog = dialog;

    // Restore: bring the entries back as open (Done = false) notes.
    buttons->AddSlot(NMkBtn("Restore", [this, weakDialog, entries, rows, dataSource, selected]() {
        if (*selected < 0 || *selected >= static_cast<int>(rows->size())) return;

        const std::string week = (*entries)[*selected].WeekTag;
        int restored = 0;
        for (const NoteArchive::NoteArchiveEntry& entry : *entries) {
            if (entry.WeekTag != week) continue;
            NoteEntry note;
            note.Done        = false;
            note.Priority    = NoteArchive::NormalizePriority(entry.Priority);
            note.Text        = entry.Text;
            note.Url         = entry.Url;
            note.Created     = entry.Created;
            note.CompletedAt.clear();
            m_Notes.push_back(note);
            ++restored;
        }

        std::vector<NoteArchive::NoteArchiveEntry> remaining;
        for (const NoteArchive::NoteArchiveEntry& entry : *entries)
            if (entry.WeekTag != week) remaining.push_back(entry);
        NoteArchive::Save(remaining);

        SaveToDisk(); RebuildList();
        m_StatusLabel->Text = "Restored " + std::to_string(restored) + " note(s) from " + week;
        if (auto d = weakDialog.lock()) d->Close();
    }));

    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));

    // Delete: drop the whole selected week from the archive.
    buttons->AddSlot(NMkBtn("Delete", [this, weakDialog, entries, rows, dataSource, selected]() {
        if (*selected < 0 || *selected >= static_cast<int>(rows->size())) return;

        const std::string week = (*entries)[*selected].WeekTag;
        std::vector<NoteArchive::NoteArchiveEntry> remaining;
        for (const NoteArchive::NoteArchiveEntry& entry : *entries)
            if (entry.WeekTag != week) remaining.push_back(entry);
        NoteArchive::Save(remaining);

        m_StatusLabel->Text = "Deleted archive week " + week;
        if (auto d = weakDialog.lock()) d->Close();
    }));

    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));

    // Copy Report: ${Content1} = selected week, ${Content2} = open TODO.
    buttons->AddSlot(NMkBtn("Copy Report", [this, weakDialog, entries, selected]() {
        std::vector<NoteArchive::NoteArchiveEntry> weekEntries;
        if (*selected >= 0 && *selected < static_cast<int>(entries->size())) {
            const std::string week = (*entries)[*selected].WeekTag;
            for (const NoteArchive::NoteArchiveEntry& entry : *entries)
                if (entry.WeekTag == week) weekEntries.push_back(entry);
        }

        std::vector<NoteEntry> pending;
        for (const NoteEntry& note : m_Notes) if (!note.Done) pending.push_back(note);
        std::stable_sort(pending.begin(), pending.end(),
            [](const NoteEntry& a, const NoteEntry& b) {
                const auto Rank = [](const std::string& p) {
                    const std::string v = NoteArchive::NormalizePriority(p);
                    return v == "High" ? 0 : (v == "Low" ? 2 : 1);
                };
                if (Rank(a.Priority) != Rank(b.Priority)) return Rank(a.Priority) < Rank(b.Priority);
                return a.Created < b.Created;
            });

        std::string todos;
        for (const NoteEntry& note : pending) todos += NoteArchive::FormatBullet(note.Text);

        const std::string report = NoteArchive::ApplyTemplate(
            NoteArchive::FormatArchiveEntries(weekEntries), todos);
        CopyTextToClipboard(report);
        m_StatusLabel->Text = "Weekly report copied to clipboard";
        if (auto d = weakDialog.lock()) d->Close();
    }));

    buttons->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));
    buttons->AddSlot(NMkBtn("Close", [weakDialog]() { if (auto d = weakDialog.lock()) d->Close(); }));

    dialog->SetButtons(buttons);
    dialog->Show();
}

inline void NotesTab::OnOpenUrl() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    auto& url = m_Notes[m_SelectedIndex].Url;
    if (url.empty()) { m_StatusLabel->Text = "No URL"; return; }
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
    m_StatusLabel->Text = "Opened: " + url;
}

inline void NotesTab::OnCopyText() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    auto& text = m_Notes[m_SelectedIndex].Text;
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
    m_StatusLabel->Text = "Copied text";
}

inline void NotesTab::OnCopyUrl() {
    if (m_SelectedIndex < 0 || m_SelectedIndex >= (int)m_Notes.size()) return;
    auto& url = m_Notes[m_SelectedIndex].Url;
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

inline void NotesTab::RebuildList() {
    std::vector<std::string> disp;
    for (auto& n : m_Notes) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s [%s] %s", n.Done ? "[DONE]" : "      ", n.Priority.c_str(), n.Text.c_str());
        disp.push_back(buf);
    }
    if (disp.empty()) disp.push_back("No notes. Click 'Add...' to create one.");
    m_DataSource->SetItems(disp);
    m_StatusLabel->Text = std::to_string(m_Notes.size()) + " note(s)";
}

inline void NotesTab::OnNoteRightClick(int32_t index, const ZSlate::Vector2& screenPos) {
    if (index < 0 || index >= (int32_t)m_Notes.size()) return;

    m_SelectedIndex = index;
    m_ListView->SetSelection(index, true);

    HWND hWnd = GetActiveWindow();
    std::vector<std::pair<std::string, int>> items = {
        {"Toggle Done",  1},
        {"Edit Priority", 2},
        {"Open URL",     3},
        {"Copy URL",     4},
        {"Copy Text",    5},
        {"-",            0},
        {"Add...",       6},
        {"Remove",       7},
        {"-",            0},
        {"Move Up",      8},
        {"Move Down",    9},
    };

    int cmd = ShowContextMenu(hWnd, (int)screenPos.x, (int)screenPos.y, items);

    switch (cmd) {
    case 1:  OnToggleDone(); break;
    case 2:  OnEdit();       break;
    case 3:  OnOpenUrl();    break;
    case 4:  OnCopyUrl();    break;
    case 5:  OnCopyText();   break;
    case 6:  OnAdd();        break;
    case 7:  OnRemove();     break;
    case 8:  OnMoveUp();     break;
    case 9:  OnMoveDown();   break;
    }
}

}
