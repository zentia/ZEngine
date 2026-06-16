#include "ZSlateConsoleWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"       // native input bus (P10)
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend (M3)
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Slate/Application/SlateApplication.h"
#include "Runtime/Slate/Widgets/SBorder.h"
#include "Runtime/Slate/Widgets/SBoxPanel.h"
#include "Runtime/Slate/Widgets/SButton.h"
#include "Runtime/Slate/Widgets/SCheckBox.h"
#include "Runtime/Slate/Widgets/SEditableTextBox.h"
#include "Runtime/Slate/Widgets/SMenu.h"
#include "Runtime/Slate/Widgets/SScrollBox.h"
#include "Runtime/Slate/Widgets/SSpacer.h"
#include "Runtime/Slate/Widgets/STextBlock.h"

#include <algorithm>
#include <bq_log/bq_log.h>
#include <cctype>
#include <cstring>
#include <mimalloc.h>
#include <string_view>

#include <GLFW/glfw3.h>

using namespace ZSlate;

namespace
{
const UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const UIColor kBarColor(0.14f, 0.14f, 0.17f, 1.0f);
const UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const UIColor kErrorColor(1.0f, 0.4f, 0.4f, 1.0f);
const UIColor kLabelColor(0.82f, 0.84f, 0.88f, 1.0f);
const UIColor kLogRowHoverColor(0.18f, 0.18f, 0.22f, 1.0f);
const UIColor kLogRowSelectedColor(0.25f, 0.30f, 0.40f, 0.75f);

UIColor LevelColor(uint32_t level)
{
    switch (level)
    {
        case 0: return UIColor(0.50f, 0.50f, 0.50f, 1.0f);  // verbose
        case 1: return UIColor(0.70f, 0.70f, 0.70f, 1.0f);  // debug
        case 2: return UIColor(0.92f, 0.93f, 0.96f, 1.0f);  // info
        case 3: return UIColor(1.00f, 0.85f, 0.20f, 1.0f);  // warning
        case 4: return UIColor(1.00f, 0.35f, 0.35f, 1.0f);  // error
        case 5: return UIColor(1.00f, 0.10f, 0.10f, 1.0f);  // fatal
        default: return UIColor(0.92f, 0.93f, 0.96f, 1.0f);
    }
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (haystack.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j)
        {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j])))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = TextAnchor::MiddleLeft;
    return t;
}
}  // namespace

ZSlateConsoleWindow* ZSlateConsoleWindow::s_Instance = nullptr;

ZSlateConsoleWindow::ZSlateConsoleWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kConsole)
{
    s_Instance = this;
    LogSystem::GetInstance().registerCallback(ConsoleCallback);
    BootstrapBufferedLogs();
}

ZSlateConsoleWindow::~ZSlateConsoleWindow()
{
    LogSystem::GetInstance().unregisterCallback(ConsoleCallback);
    s_Instance = nullptr;

    std::lock_guard<std::mutex> lock(m_LogMutex);
    FreeEntries(m_LogEntries);
    FreeEntries(m_PendingEntries);
}

void ZSlateConsoleWindow::FreeEntries(std::vector<LogEntry>& entries)
{
    for (auto& entry : entries)
    {
        if (entry.m_Content != nullptr)
        {
            mi_free(entry.m_Content);
            entry.m_Content = nullptr;
            entry.m_Length = 0;
        }
    }
    entries.clear();
}

void BQ_STDCALL ZSlateConsoleWindow::ConsoleCallback(uint64_t log_id,
                                                     int32_t category_idx,
                                                     bq::log_level log_level,
                                                     const char* content,
                                                     int32_t length)
{
    if (s_Instance == nullptr)
        return;

    std::lock_guard<std::mutex> lock(s_Instance->m_LogMutex);
    auto& entry = s_Instance->m_PendingEntries.emplace_back();
    entry.m_LogId = log_id;
    entry.m_CategoryIdx = static_cast<uint32_t>(category_idx);
    entry.m_LogLevel = static_cast<uint32_t>(log_level);

    if (length > 0 && content != nullptr && length <= 1024 * 1024)
    {
        entry.m_Content = static_cast<char*>(mi_malloc(static_cast<size_t>(length) + 1));
        memcpy(entry.m_Content, content, static_cast<size_t>(length));
        entry.m_Content[length] = '\0';
        entry.m_Length = length;
    }
    else
    {
        entry.m_Content = nullptr;
        entry.m_Length = 0;
    }
}

void ZSlateConsoleWindow::DrainPending()
{
    std::lock_guard<std::mutex> lock(m_LogMutex);
    if (m_PendingEntries.empty())
        return;
    m_LogEntries.insert(m_LogEntries.end(),
                        std::make_move_iterator(m_PendingEntries.begin()),
                        std::make_move_iterator(m_PendingEntries.end()));
    m_PendingEntries.clear();
}

void ZSlateConsoleWindow::PollConsoleBuffer()
{
    while (bq::log::fetch_and_remove_console_buffer(&ConsoleCallback))
    {
    }
}

void ZSlateConsoleWindow::PumpBufferedLogsIfOpen()
{
    if (s_Instance == nullptr || !s_Instance->m_Open)
        return;
    s_Instance->PollConsoleBuffer();
    s_Instance->DrainPending();
}

void ZSlateConsoleWindow::BootstrapBufferedLogs()
{
    PollConsoleBuffer();
    DrainPending();
    m_FilterDirty = true;
}

void ZSlateConsoleWindow::ExecuteCommand(const std::string& command)
{
    if (command.empty())
        return;
    if (auto console = GET_SYSTEM(ConsoleManager))
        console->ExecuteString(command.c_str());
}

void ZSlateConsoleWindow::RebuildCategoryOptions()
{
    m_CategoryLabels.clear();
    m_CategoryIndices.clear();
    m_CategoryLabels.push_back("All");
    m_CategoryIndices.push_back(-1);

    if (LogSystem::GetInstance().m_Logger != nullptr)
    {
        const bq::array<bq::string>& names = LogSystem::GetInstance().m_Logger->get_categories_name_array();
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i].is_empty())
                continue;
            m_CategoryLabels.push_back(names[i].c_str());
            m_CategoryIndices.push_back(static_cast<int32_t>(i));
        }
    }

    m_CategorySelection = 0;
    m_CategoryOptionsBuilt = true;
}

int ZSlateConsoleWindow::ActiveCategoryIndex() const
{
    if (m_CategorySelection < 0 || m_CategorySelection >= static_cast<int>(m_CategoryIndices.size()))
        return -1;
    return m_CategoryIndices[static_cast<size_t>(m_CategorySelection)];
}

void ZSlateConsoleWindow::RebuildSearchMatcher()
{
    m_SearchActive = !m_Search.empty();
    if (!m_SearchActive)
    {
        m_RegexValid = false;
        m_CompiledKey.clear();
        return;
    }

    const std::string key = m_Search + (m_UseRegex ? "|rx" : "|txt") + (m_CaseSensitive ? "|cs" : "|ci");
    if (key == m_CompiledKey)
        return;
    m_CompiledKey = key;

    if (!m_UseRegex)
    {
        m_RegexValid = false;
        return;
    }

    try
    {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (!m_CaseSensitive)
            flags |= std::regex::icase;
        m_Regex = std::regex(m_Search, flags);
        m_RegexValid = true;
    }
    catch (const std::regex_error&)
    {
        m_RegexValid = false;
    }
}

bool ZSlateConsoleWindow::EntryMatchesFilter(const LogEntry& entry) const
{
    const int category_filter = ActiveCategoryIndex();
    if (category_filter >= 0 && static_cast<int32_t>(entry.m_CategoryIdx) != category_filter)
        return false;

    if (!m_SearchActive)
        return true;
    if (entry.m_Content == nullptr || entry.m_Length <= 0)
        return false;

    const std::string_view content(entry.m_Content, static_cast<size_t>(entry.m_Length));
    if (m_UseRegex)
    {
        if (!m_RegexValid)
            return true;  // invalid regex -> show everything (matches ImGui console)
        return std::regex_search(content.begin(), content.end(), m_Regex);
    }
    if (m_CaseSensitive)
        return content.find(m_Search) != std::string_view::npos;
    return ContainsCaseInsensitive(content, m_Search);
}

std::shared_ptr<SButton> ZSlateConsoleWindow::MakeLogRow(size_t entry_index, const LogEntry& entry, bool selected)
{
    const float font_size = 13.0f * (m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f);
    std::string text;
    if (entry.m_Content != nullptr && entry.m_Length > 0)
        text.assign(entry.m_Content, static_cast<size_t>(entry.m_Length));

    auto row = std::make_shared<SButton>();
    row->Padding = FMargin(2.0f, 1.0f);
    row->NormalColor = selected ? kLogRowSelectedColor : UIColor(0.0f, 0.0f, 0.0f, 0.0f);
    row->HoverColor = kLogRowHoverColor;
    row->PressedColor = kLogRowSelectedColor;
    row->HAlign = EHorizontalAlignment::Fill;
    row->SetContent(MakeText(text, font_size, LevelColor(entry.m_LogLevel)));
    row->OnClicked = [this, entry_index]() {
        const int idx = static_cast<int>(entry_index);
        if (m_SelectedEntryIndex != idx)
        {
            m_SelectedEntryIndex = idx;
            UpdateLogRowSelectionHighlight();
        }
    };
    row->OnRightClicked = [this, entry_index](const Vector2& screen_pos) {
        m_PendingContextEntryIndex = static_cast<int>(entry_index);
        m_PendingContextPos = screen_pos;
        m_HasPendingContext = true;
    };
    m_LogRowWidgets.push_back({entry_index, row});
    return row;
}

void ZSlateConsoleWindow::UpdateLogRowSelectionHighlight()
{
    for (const LogRowWidget& row_widget : m_LogRowWidgets)
    {
        if (!row_widget.button)
            continue;
        const bool selected = static_cast<int>(row_widget.entry_index) == m_SelectedEntryIndex;
        row_widget.button->NormalColor =
            selected ? kLogRowSelectedColor : UIColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

void ZSlateConsoleWindow::RebuildLogRows()
{
    if (!m_LogList)
        return;
    // Log rows are destroyed below; drop stale hover pointers but keep keyboard
    // focus on the search/command box (Reset() was clearing focus every keystroke).
    m_Input.ClearHoverPath();
    m_LogList->ClearChildren();
    m_LogRowWidgets.clear();
    for (size_t i = 0; i < m_LogEntries.size(); ++i)
    {
        if (EntryMatchesFilter(m_LogEntries[i]))
            m_LogList->AddChild(MakeLogRow(i, m_LogEntries[i], static_cast<int>(i) == m_SelectedEntryIndex));
    }
    m_LastBuiltCount = m_LogEntries.size();
}

void ZSlateConsoleWindow::AppendNewLogRows()
{
    if (!m_LogList)
        return;
    for (size_t i = m_LastBuiltCount; i < m_LogEntries.size(); ++i)
    {
        if (EntryMatchesFilter(m_LogEntries[i]))
            m_LogList->AddChild(MakeLogRow(i, m_LogEntries[i], static_cast<int>(i) == m_SelectedEntryIndex));
    }
    m_LastBuiltCount = m_LogEntries.size();
}

void ZSlateConsoleWindow::CopySelectedLogToClipboard() const
{
    if (m_SelectedEntryIndex < 0 || m_SelectedEntryIndex >= static_cast<int>(m_LogEntries.size()))
        return;

    const LogEntry& entry = m_LogEntries[static_cast<size_t>(m_SelectedEntryIndex)];
    if (entry.m_Content == nullptr || entry.m_Length <= 0)
        return;

    if (auto* window_system = GET_SYSTEM(WindowSystem))
    {
        if (GLFWwindow* glfw_window = window_system->GetWindow())
        {
            glfwSetClipboardString(glfw_window, entry.m_Content);
        }
    }
}

void ZSlateConsoleWindow::BuildLayout(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = FMargin(4.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();

    // ---- Top bar: category | search | regex | case | clear | count ----------
    auto bar = std::make_shared<SHorizontalBox>();

    m_CategoryLabel = MakeText("All", font, kLabelColor);
    m_CategoryButton = std::make_shared<SButton>();
    m_CategoryButton->Padding = FMargin(8.0f * scale, 3.0f * scale);
    m_CategoryButton->SetContent(m_CategoryLabel);
    m_CategoryButton->OnClicked = [this]() { OpenCategoryMenu(); };
    bar->AddSlot(m_CategoryButton).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();

    m_SearchBox = std::make_shared<SEditableTextBox>();
    m_SearchBox->FontSize = font;
    m_SearchBox->HintText = m_UseRegex ? "Search logs (regex)..." : "Search logs...";
    m_SearchBox->MinWidth = 120.0f * scale;
    m_SearchBox->OnTextChanged = [this](const std::string& t) {
        m_Search = t;
        m_FilterDirty = true;
    };
    bar->AddSlot(m_SearchBox).Fill().SetVAlign(EVerticalAlignment::Center);

    auto add_check = [&](const char* label, bool initial, std::function<void(bool)> on_change) {
        bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();
        auto cb = std::make_shared<SCheckBox>();
        cb->Checked = initial;
        cb->BoxSize = 16.0f * scale;
        cb->OnCheckStateChanged = std::move(on_change);
        bar->AddSlot(cb).AutoSize().SetVAlign(EVerticalAlignment::Center);
        bar->AddSlot(std::make_shared<SSpacer>(Vector2(3.0f * scale, 0.0f))).AutoSize();
        bar->AddSlot(MakeText(label, font, kLabelColor)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    };
    add_check("Regex", m_UseRegex, [this](bool b) {
        m_UseRegex = b;
        if (m_SearchBox)
            m_SearchBox->HintText = b ? "Search logs (regex)..." : "Search logs...";
        m_FilterDirty = true;
    });
    add_check("Aa", m_CaseSensitive, [this](bool b) {
        m_CaseSensitive = b;
        m_FilterDirty = true;
    });

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();
    auto clear_btn = std::make_shared<SButton>();
    clear_btn->Padding = FMargin(8.0f * scale, 3.0f * scale);
    clear_btn->SetContent(MakeText("Clear", font, kLabelColor));
    clear_btn->OnClicked = [this]() {
        m_Search.clear();
        if (m_SearchBox)
            m_SearchBox->Text.clear();
        m_SelectedEntryIndex = -1;
        m_FilterDirty = true;
    };
    bar->AddSlot(clear_btn).AutoSize().SetVAlign(EVerticalAlignment::Center);

    bar->AddSlot(std::make_shared<SSpacer>(Vector2(6.0f * scale, 0.0f))).AutoSize();
    m_CountText = MakeText("", font, kDimColor);
    bar->AddSlot(m_CountText).AutoSize().SetVAlign(EVerticalAlignment::Center);

    column->AddSlot(bar).AutoSize().SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Log list ----------------------------------------------------------
    auto log_border = std::make_shared<SBorder>();
    log_border->BackgroundColor = UIColor(0.07f, 0.07f, 0.09f, 1.0f);
    log_border->Padding = FMargin(4.0f * scale, 4.0f * scale);
    log_border->HAlign = EHorizontalAlignment::Fill;
    log_border->VAlign = EVerticalAlignment::Fill;

    m_LogList = std::make_shared<SScrollBox>();
    log_border->SetContent(m_LogList);
    column->AddSlot(log_border).Fill();

    // ---- Command input ------------------------------------------------------
    m_CommandBox = std::make_shared<SEditableTextBox>();
    m_CommandBox->FontSize = font;
    m_CommandBox->HintText = "Enter console command...";
    m_CommandBox->BackgroundColor = kBarColor;
    m_CommandBox->OnTextCommitted = [this](const std::string& t) {
        ExecuteCommand(t);
        if (m_CommandBox)
            m_CommandBox->Text.clear();
        m_Command.clear();
    };
    m_CommandBox->OnTextChanged = [this](const std::string& t) { m_Command = t; };
    column->AddSlot(m_CommandBox).AutoSize().SetPadding(FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));

    root->SetContent(column);
    m_Root = root;
}

void ZSlateConsoleWindow::OpenCategoryMenu()
{
    const float scale = m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f;
    if (m_Menu == nullptr)
        m_Menu = std::make_shared<SMenu>();
    m_Menu->Clear();
    m_Menu->MinWidth = 160.0f * scale;

    for (int i = 0; i < static_cast<int>(m_CategoryLabels.size()); ++i)
    {
        const std::string label = m_CategoryLabels[static_cast<size_t>(i)];
        m_Menu->AddItem(label, [this, i, label]() {
            m_CategorySelection = i;
            if (m_CategoryLabel)
                m_CategoryLabel->Text = label;
            m_FilterDirty = true;
        }, scale);
    }

    // Anchor under the category button.
    const FGeometry& g = m_CategoryButton->GetCachedGeometry();
    m_MenuPos = Vector2(g.AbsolutePosition.x, g.AbsolutePosition.y + g.LocalSize.y);
    m_MenuOpen = true;
    m_MenuInput.Reset();
}

void ZSlateConsoleWindow::OpenLogContextMenu(size_t entry_index, const Vector2& screen_pos)
{
    m_SelectedEntryIndex = static_cast<int>(entry_index);
    UpdateLogRowSelectionHighlight();

    const float scale = m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f;
    if (m_Menu == nullptr)
        m_Menu = std::make_shared<SMenu>();
    m_Menu->Clear();
    m_Menu->MinWidth = 120.0f * scale;

    const bool can_copy = entry_index < m_LogEntries.size() && m_LogEntries[entry_index].m_Content != nullptr &&
                          m_LogEntries[entry_index].m_Length > 0;
    m_Menu->AddItem("Copy", [this]() { CopySelectedLogToClipboard(); }, scale, !can_copy);

    m_MenuPos = screen_pos;
    m_MenuOpen = true;
    m_MenuInput.Reset();
}

void ZSlateConsoleWindow::CloseMenu()
{
    m_MenuOpen = false;
    m_MenuInput.Reset();
}

void ZSlateConsoleWindow::OnGUI()
{
    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    if (!m_CategoryOptionsBuilt)
        RebuildCategoryOptions();

    if (m_Root == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildLayout(ui_scale);
        m_FilterDirty = true;
        m_LastBuiltCount = 0;
        m_Input.Reset();
    }

    PollConsoleBuffer();
    DrainPending();
    RebuildSearchMatcher();

    // Auto-scroll follows the bottom unless the user scrolled up.
    if (m_LogList)
        m_AutoScroll = m_LogList->IsAtBottom();

    if (m_FilterDirty)
    {
        RebuildLogRows();
        m_FilterDirty = false;
        if (m_LogList)
            m_LogList->ScrollToBottom();
    }
    else if (m_LogEntries.size() > m_LastBuiltCount)
    {
        AppendNewLogRows();
        if (m_AutoScroll && m_LogList)
            m_LogList->ScrollToBottom();
    }

    // Count + regex-error indicator.
    if (m_CountText)
    {
        const size_t shown = m_LogList ? static_cast<size_t>(m_LogList->GetChildCount()) : 0;
        m_CountText->Text = std::to_string(shown) + "/" + std::to_string(m_LogEntries.size());
        const bool regex_bad = m_SearchActive && m_UseRegex && !m_RegexValid;
        if (regex_bad)
            m_CountText->Text += " (invalid regex)";
        m_CountText->Color = regex_bad ? kErrorColor : kDimColor;
    }

    // ---- Paint --------------------------------------------------------------
    // P10c: native-host panels source their leaf rect from EditorView::NativeRect()
    // (no ImGui::Begin / item to probe); otherwise use the ImGui content region.
    const float* native_rect = NativeRect();
    Vector2 pos(native_rect[0], native_rect[1]);
    Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(Vector2(pos.x, pos.y), Vector2(avail.x, avail.y));

    // P9: the native RHI backend paints into the shared BatchedUIRenderer (frame managed by
    // ZSlateEditorOverlay around WindowUI::PreRender), clipped to this panel. The legacy
    // per-window SlateImGuiRenderer fallback was retired.
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        renderer.pushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.popClipRect();
    }

    // ---- Input --------------------------------------------------------------
    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed
    // EditorSlateHost (the transitional r.ZSlate.NativeInput CVar was retired in
    // P10c, so the old ImGui::GetIO() fallback branches were dead and are gone).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const bool right_down = host.IsRightDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;
    const bool left_edge = left_down && !m_PrevLeftDown;
    const bool right_up_edge = !right_down && m_PrevRightDown;

    if (m_MenuOpen && m_Menu)
    {
        m_Menu->CacheDesiredSize();
        const Vector2 menu_size = m_Menu->GetDesiredSize();
        // Clamp the menu to the native display rect (== the editor's full-window
        // viewport work area).
        const Vector2 disp_pos = host.GetDisplayPos();
        const Vector2 disp_size = host.GetDisplaySize();
        const float vx = disp_pos.x;
        const float vy = disp_pos.y;
        const float vw = disp_size.x;
        const float vh = disp_size.y;
        float mx = std::min(m_MenuPos.x, vx + vw - menu_size.x);
        float my = std::min(m_MenuPos.y, vy + vh - menu_size.y);
        mx = std::max(mx, vx);
        my = std::max(my, vy);
        const FGeometry menu_geom(Vector2(mx, my), menu_size);

        {
            // Append the popup into the shared batch after the panel content so it
            // draws on top (the native overlay is composited after all ImGui).
            BatchedUIRenderer& renderer = overlay.GetRenderer();
            FPaintContext menu_ctx;
            menu_ctx.Renderer = &renderer;
            menu_ctx.LayerId = 1;
            m_Menu->Paint(menu_ctx, menu_geom);
        }

        if (left_edge && !menu_geom.IsUnderLocation(mouse))
        {
            CloseMenu();
        }
        else
        {
            m_MenuInput.ProcessMouse(m_Menu, mouse, true, left_down, wheel);
            if (m_Menu->IsCloseRequested())
                CloseMenu();
        }
    }
    else
    {
        m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel, right_down);

        if (right_up_edge && over_canvas && m_HasPendingContext)
        {
            if (m_PendingContextEntryIndex >= 0 &&
                m_PendingContextEntryIndex < static_cast<int>(m_LogEntries.size()))
            {
                OpenLogContextMenu(static_cast<size_t>(m_PendingContextEntryIndex), m_PendingContextPos);
            }
            m_HasPendingContext = false;
        }

        if (over_canvas && !m_Input.HasKeyboardFocus() && m_SelectedEntryIndex >= 0)
        {
            bool copy_shortcut = false;
            if (auto* window_system = GET_SYSTEM(WindowSystem))
            {
                if (GLFWwindow* glfw_window = window_system->GetWindow())
                {
                    const bool ctrl = host.IsCtrlDown();
                    const bool c_down = glfwGetKey(glfw_window, GLFW_KEY_C) == GLFW_PRESS;
                    const bool shortcut_down = ctrl && c_down;
                    copy_shortcut = shortcut_down && !m_PrevCopyShortcut;
                    m_PrevCopyShortcut = shortcut_down;
                }
            }
            if (copy_shortcut)
                CopySelectedLogToClipboard();
        }
        else
        {
            m_PrevCopyShortcut = false;
        }

        const bool text_input_active = m_Input.HasKeyboardFocus() ||
                                       (m_CommandBox && m_CommandBox->IsFocused()) ||
                                       (m_SearchBox && m_SearchBox->IsFocused());
        if (text_input_active)
        {
            host.NotifyNativeTextInputActive();

            // Update modifier state so widgets can query Shift/Ctrl/Alt
            ZSlate::SlateInputRouter::SetModifierState(
                host.IsCtrlDown(), host.IsShiftDown(), host.IsAltDown());

            for (unsigned int cp : host.GetCharsThisFrame())
                m_Input.ProcessChar(cp);
            // UE pattern: route ALL keys to the focused widget; the widget's
            // OnKeyDown decides whether to handle or ignore each key.
            for (EKey key : host.GetKeysThisFrame())
                m_Input.ProcessKey(key);
        }
    }

    m_PrevLeftDown = left_down;
    m_PrevRightDown = right_down;
}
