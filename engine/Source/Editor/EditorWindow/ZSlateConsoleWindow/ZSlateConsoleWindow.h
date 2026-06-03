#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlateGeometry.h"  // Vector2

#include <bq_log/misc/bq_log_def.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SEditableTextBox;
class STextBlock;
class SButton;
class SMenu;
}  // namespace ZSlate

// One buffered BqLog record. Owned by ZSlateConsoleWindow's log model; m_Content
// is allocated with mi_malloc and freed with mi_free in FreeEntries(). Relocated
// here from the deleted legacy ConsoleWindow (the only remaining consumer).
class LogEntry
{
public:
    uint64_t m_LogId;
    uint32_t m_CategoryIdx;
    uint32_t m_LogLevel;
    char* m_Content {nullptr};  // Allocated with mi_malloc, freed with mi_free
    int32_t m_Length {0};
};

// A ZSlate-rendered, dockable Console (default panel at dock title "Console").
// Streams BqLog output into a scrollable list coloured by level, with a category
// dropdown, a regex/case search filter, a visible/total counter, and a command
// input line wired to ConsoleManager.
//
// Layout is built ONCE and kept alive so the search/command text boxes keep focus
// while logs stream; only the log rows inside the SScrollBox are (incrementally)
// rebuilt. Tab autocomplete / Up-Down history are not implemented yet.
class ZSlateConsoleWindow : public EditorWindow
{
public:
    explicit ZSlateConsoleWindow(EditorUI* editor_ui);
    ~ZSlateConsoleWindow() override;
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    static void BQ_STDCALL
    ConsoleCallback(uint64_t log_id, int32_t category_idx, bq::log_level log_level, const char* content, int32_t length);

private:
    void BuildLayout(float scale);
    void RebuildLogRows();                 // full rebuild (filter changed)
    void AppendNewLogRows();               // incremental (only new entries)
    std::shared_ptr<ZSlate::STextBlock> MakeLogRow(const LogEntry& entry) const;
    bool EntryMatchesFilter(const LogEntry& entry) const;
    int ActiveCategoryIndex() const;
    void RebuildCategoryOptions();
    void RebuildSearchMatcher();
    void OpenCategoryMenu();
    void CloseMenu();
    void ExecuteCommand(const std::string& command);
    void DrainPending();
    void BootstrapBufferedLogs();
    void FreeEntries(std::vector<LogEntry>& entries);

    static ZSlateConsoleWindow* s_Instance;

    // Log model (own buffer, thread-safe append from the BqLog callback thread).
    std::mutex m_LogMutex;
    std::vector<LogEntry> m_LogEntries;
    std::vector<LogEntry> m_PendingEntries;

    // Filter state.
    std::string m_Search;
    bool m_UseRegex {true};
    bool m_CaseSensitive {false};
    bool m_SearchActive {false};
    bool m_RegexValid {false};
    std::string m_CompiledKey;
    std::regex m_Regex;

    int m_CategorySelection {0};
    std::vector<std::string> m_CategoryLabels;
    std::vector<int32_t> m_CategoryIndices;
    bool m_CategoryOptionsBuilt {false};

    // Command input.
    std::string m_Command;

    // Rebuild bookkeeping.
    bool m_FilterDirty {true};
    size_t m_LastBuiltCount {0};
    float m_BuiltScale {-1.0f};
    bool m_AutoScroll {true};

    // ZSlate tree (persistent; only the log rows are rebuilt).
    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SScrollBox> m_LogList;
    std::shared_ptr<ZSlate::SEditableTextBox> m_SearchBox;
    std::shared_ptr<ZSlate::SEditableTextBox> m_CommandBox;
    std::shared_ptr<ZSlate::STextBlock> m_CountText;
    std::shared_ptr<ZSlate::SButton> m_CategoryButton;
    std::shared_ptr<ZSlate::STextBlock> m_CategoryLabel;

    // Category dropdown overlay (per-window, foreground-painted popup).
    std::shared_ptr<ZSlate::SMenu> m_Menu;
    ZSlate::SlateInputRouter m_MenuInput;
    Vector2 m_MenuPos {0.0f, 0.0f};
    bool m_MenuOpen {false};

    ZSlate::SlateInputRouter m_Input;

    bool m_PrevLeftDown {false};
};
