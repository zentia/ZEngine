// ============================================================================
// ZSlate front-end of ZEnvironmentCollector -- three tabs over the one core.
// ============================================================================
//   tab 0  Collection          -- safe collection + crash capture
//   tab 1  Compatibility       -- the old CLI's plain check, run through the same
//                                 EnvironmentCheckCommand pipeline that -batchmode
//                                 uses
//   tab 2  GPU Selection       -- the old ZGpuSelectionInspector
//
// User-visible text is English in the source and looked up through ZSlate's
// localization (ZSlate/Core/SlateLocalization.h); the Chinese table lives in
// UiLocalization.cpp. Default is English, the choice is remembered per user, and
// --lang en|zh-CN overrides it for one run.
//
// This file keeps its own window class, WindowProc and DPI plumbing instead of
// ZSlate::FSlateStandaloneApp, for the same reason the collector always did: a
// Unicode window, capture acknowledged through WM_CLOSE while a background job
// is still running, combo popups that take mouse and key events before the
// widget tree, and a `busy` gate that drops all input. What is shared with the
// other ZSlate tools is still shared: font discovery
// (ZSlate::Win32::LoadSystemFonts) and DPI-sized window framing
// (ZSlate::Win32::ResizeWindowForDpi).
//
// Widget metrics stay in 96-DPI logical units; `scaled` converts to physical
// pixels on the way to the renderer and snaps edges to whole pixels. Win32
// coordinates are physical and are divided by `dpiScale` at the WindowProc
// boundary (see ShellApp::Logical).
// ============================================================================

#include "EnvironmentCheckGui.h"

#include "Collection.h"
#include "CrashCapture.h"
#include "EnvCheckLog.h"
#include "WerCapture.h"
#include "ToolVersion.h"
#include "DiagnosisUtil.h"
#include "EnvironmentCheckCommand.h"
#include "GpuSelectionProbe.h"
#include <commdlg.h>
#include <shlobj.h>
#include "UiLocalization.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateLocalization.h"
#include "ZSlate/Core/SlateLog.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Backend/SlateUIRendererBackend.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Platform/Win32/SlateWin32Dpi.h"
#include "ZSlate/Platform/Win32/SlateWin32Host.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SScrollPanel.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SComboBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include <windowsx.h>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace ZSlate;
using namespace zengine::envcheck::collector;
using namespace zengine::envcheck::diagnosis;

constexpr int TabCount = 3;
// Long reports are cut here so a pathological machine cannot allocate tens of
// thousands of STextBlocks; the full text is always on disk.
constexpr int MaxReportLines = 600;

const UIColor PassColor(0.40f, 0.85f, 0.40f, 1.0f);
const UIColor FailColor(0.90f, 0.35f, 0.35f, 1.0f);
const UIColor DimColor(0.66f, 0.66f, 0.70f, 1.0f);
const UIColor AccentColor(0.60f, 0.70f, 0.90f, 1.0f);
const UIColor ActiveTabColor(0.16f, 0.34f, 0.55f, 1.0f);
const UIColor IdleTabColor(0.20f, 0.20f, 0.22f, 1.0f);

enum class ShellTab
{
    Collection = 0,
    Compatibility = 1,
    GpuSelection = 2,
};

// Every user-visible string goes through ZSlate's localization. The English
// text is the one written here; Chinese comes from the table in
// UiLocalization.cpp, and a missing entry falls back to what is written here.
const char* S(const char* id, const char* english) { return ZSlate::Loc(id, english); }
std::wstring W(const char* id, const char* english) { return Utf8ToWide(S(id, english)); }

// Which background job is running. Replaces the old `capturing` bool: the shell
// runs three different jobs and Update() has to know which one just ended.
enum class Job
{
    None,
    Collection,
    Capture,
    WerRestore,
    Compatibility,
};

// Only used to label log lines -- the UI never shows it.
const char* JobName(Job job)
{
    switch (job)
    {
    case Job::Collection:    return "collection";
    case Job::Capture:       return "capture";
    case Job::WerRestore:    return "wer-restore";
    case Job::Compatibility: return "compatibility";
    case Job::None:          break;
    }
    return "idle";
}

class Platform final : public ISlatePlatform
{
public:
    ZSlateBatchedRenderer renderer;
    FSlateDpiScaleRenderer scaled{&renderer, 1.0f, true};
    Vector2 mouse{0,0}, size{920,680};  // logical
    float dpiScale = 1;
    bool down = false;
    ISlateRenderer* GetRenderer() override { return &scaled; }
    Vector2 GetMousePosition() const override { return mouse; }
    bool IsMouseButtonDown(int button) const override { return button == 0 && down; }
    bool IsKeyDown(int key) const override { return (GetKeyState(key) & 0x8000) != 0; }
    float GetTimeSeconds() const override { return static_cast<float>(GetTickCount64() % 10000000) / 1000; }
    Vector2 GetWindowSize() const override { return size; }
    float GetDpiScale() const override { return dpiScale; }
};

class ShellApp
{
public:
    Platform platform;
    ZSlateFontAtlas fonts;
    // Layout measurer: without it STextBlock falls back to a per-BYTE estimate
    // (UTF-8 CJK chars count as three), which inflates the column's desired
    // width ~2.5x and stretches every Fill-width button past the window edge.
    // Measuring through the scaled renderer keeps the numbers in logical units
    // and identical to what DrawTextLabel will actually paint.
    ZSlate::SlateUIRendererTextMeasurer measurer;
    SlateInputRouter router;
    Options options;
    HWND window = nullptr;
    std::unique_ptr<ZSlateD3D11Renderer> backend;

    ShellTab tab = ShellTab::Collection;
    Job job = Job::None;
    std::shared_ptr<SVerticalBox> column;                 // column currently being built
    std::array<std::shared_ptr<SVerticalBox>, TabCount> tabs;
    std::vector<std::shared_ptr<SButton>> tabButtons;
    std::vector<std::shared_ptr<STextBlock>> tabLabels;
    std::shared_ptr<SScrollBox> scroll;
    // Everything the `busy` gate greys out, across all three tabs.
    std::vector<std::shared_ptr<SWidget>> interactive;

    std::shared_ptr<STextBlock> status, timeLabel;
    // Shown above the collection tab whenever the package would come back
    // unattributable (missing incident time / incident kind / game exe).
    std::shared_ptr<STextBlock> anchorWarning;
    std::shared_ptr<SComboBox<int>> langBox;
    std::shared_ptr<SComboBox<IncidentKind>> kindBox;
    std::shared_ptr<SComboBox<int>> captureMode;
    std::shared_ptr<SEditableTextBox> timeInput;
    std::shared_ptr<SCheckBox> details;
    std::filesystem::path procDumpPath, targetPath;
    std::shared_ptr<STextBlock> procDumpLabel, targetLabel, captureStatus;
    std::string captureMessage;

    std::shared_ptr<SEditableTextBox> checkOutputInput, checkRequireInput, checkMemoryInput;
    std::shared_ptr<SCheckBox> checkDllBox;
    std::shared_ptr<STextBlock> checkStatus;
    std::shared_ptr<SVerticalBox> checkReportBox;
    zengine::envcheck::EnvironmentCheckOutcome checkOutcome;

    std::shared_ptr<STextBlock> gpuStatus;
    std::shared_ptr<SVerticalBox> gpuReportBox;

    std::atomic_bool cancel{false}, finished{false}, cancellable{false};
    std::thread worker;
    std::mutex mutex;
    std::string progress, error;
    Result result;
    Result captureReport;
    bool busy = false, closing = false, minimized = false;
    bool suppressPointerUntilRelease = false;

    ~ShellApp()
    {
        cancel = true;
        if (worker.joinable()) worker.join();
        SlateApplication::Get().SetTextMeasurer(nullptr);
        SlateApplication::Get().SetRootContent(nullptr);
        SetPlatform(nullptr);
    }
    void AddWidget(const std::shared_ptr<SWidget>& widget, float padding)
    {
        column->AddSlot(widget).AutoSize().SetPadding(FMargin(22, padding));
    }
    // Only the scale and the logical client size change; widget metrics stay in
    // logical units and FSlateDpiScaleRenderer applies the new factor on paint.
    void ApplyDpi(UINT dpi)
    {
        // TEMPORARY debug hook: reproduce a user's high-DPI rendering on a
        // 96-DPI dev machine. Remove once the glyph-scramble report is solved.
        if (char buf[16] = {}; GetEnvironmentVariableA("ZCOLLECTOR_FORCE_DPI", buf, sizeof(buf)) > 0)
        {
            const int forced = std::atoi(buf);
            if (forced >= 96 && forced <= 480)
            {
                ZSlate::Log(ZSlate::ELogLevel::Info, "[debug] forcing DPI %d (queried %u)", forced, dpi);
                dpi = static_cast<UINT>(forced);
            }
        }
        platform.dpiScale = static_cast<float>(dpi ? dpi : 96) / 96;
        platform.scaled.SetScale(platform.dpiScale);
        RECT client{};
        if (window && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
            SetPhysicalSize(client.right, client.bottom);
        if (kindBox) kindBox->ClosePopup();
        if (captureMode) captureMode->ClosePopup();
        if (langBox) langBox->ClosePopup();
        ResetPointerGesture();
    }
    void SetPhysicalSize(int width, int height)
    {
        platform.size = Vector2(static_cast<float>(width) / platform.dpiScale, static_cast<float>(height) / platform.dpiScale);
    }
    Vector2 Logical(int x, int y) const
    {
        return Vector2(static_cast<float>(x) / platform.dpiScale, static_cast<float>(y) / platform.dpiScale);
    }
    void ResetPointerGesture()
    {
        suppressPointerUntilRelease = platform.down;
        router.ResetPointerState();
    }
    std::shared_ptr<STextBlock> Text(const std::string& text, float size = 17)
    {
        auto w = std::make_shared<STextBlock>(); w->SetText(text); w->SetFontSize(size);
        AddWidget(w, 7);
        return w;
    }
    std::shared_ptr<STextBlock> Button(const std::string& text, const std::function<void()>& callback)
    {
        auto button = std::make_shared<SButton>();
        auto label = std::make_shared<STextBlock>(); label->SetText(text); label->SetFontSize(21);
        button->SetContent(label); button->OnClicked = callback;
        AddWidget(button, 9);
        return label;
    }
    void Interactive(const std::shared_ptr<SWidget>& widget)
    {
        interactive.push_back(widget);
    }
    // The candidate list, the Fonts directory lookup and the CJK fallback chain
    // are ZSlate::Win32::LoadSystemFonts() now -- this tool used to be the only
    // one of the five that derived the directory instead of hardcoding
    // "C:\Windows\Fonts", and that version is the one that got promoted.
    bool InitFonts()
    {
        if (!Win32::LoadSystemFonts(fonts, Win32::ESystemFont::Sans)) return false;
        platform.renderer.SetFontAtlas(&fonts);
        measurer.SetRenderer(&platform.scaled);
        SlateApplication::Get().SetTextMeasurer(&measurer);
        return true;
    }
    void UpdateWindowTitle()
    {
        if (window) SetWindowTextW(window, W("window.title", "Environment Collector - ZSlate").c_str());
    }
    void ApplyLanguage(ELanguage language)
    {
        if (language == GetLanguage()) return;
        SetLanguage(language);
        zengine::envcheck::LogInfo(std::string("UI language -> ") + LanguageId(language));
        // Remembered per user, next to the ProcDump path: an operator who picked
        // Chinese once should not have to pick it again on every launch.
        zengine::envcheck::ui::SaveLanguagePreference(LanguageId(language));
        UpdateWindowTitle();
        RebuildUi();
    }
    // Strings are resolved when a widget is built, not when it is painted, so a
    // language switch means rebuilding the tree. Everything the user typed or
    // picked is carried across -- switching language must not quietly reset the
    // form they were filling in.
    void RebuildUi()
    {
        const ShellTab current = tab;
        const IncidentKind kind = kindBox ? kindBox->SelectedItem : options.incidentKind;
        const int mode = captureMode ? captureMode->SelectedIndex : 0;
        const std::string time = timeInput ? timeInput->GetText() : std::string();
        const std::string output = checkOutputInput ? checkOutputInput->GetText() : std::string();
        const std::string isa = checkRequireInput ? checkRequireInput->GetText() : std::string();
        const std::string memory = checkMemoryInput ? checkMemoryInput->GetText() : std::string();
        const bool events = details && details->IsChecked();
        const bool dll = checkDllBox && checkDllBox->IsChecked();

        interactive.clear();
        tabButtons.clear();
        tabLabels.clear();
        Build();

        options.incidentKind = kind;
        if (kindBox) { kindBox->SelectedItem = kind; kindBox->SelectedIndex = static_cast<int32_t>(kind); }
        if (captureMode && mode >= 0 && mode < static_cast<int>(captureMode->Options.size()))
        { captureMode->SelectedIndex = mode; captureMode->SelectedItem = captureMode->Options[static_cast<std::size_t>(mode)]; }
        if (timeInput) timeInput->SetText(time);
        // After the time box is restored: the warning reads it, and during
        // Build() it still held the pre-rebuild (empty) text.
        RefreshAnchorWarning();
        if (checkOutputInput) checkOutputInput->SetText(output);
        if (checkRequireInput) checkRequireInput->SetText(isa);
        if (checkMemoryInput) checkMemoryInput->SetText(memory);
        if (details) details->SetChecked(events);
        if (checkDllBox) checkDllBox->SetChecked(dll);
        SelectTab(current);
    }
    void Build()
    {
        auto strip = std::make_shared<SHorizontalBox>();
        const std::array<const char*, TabCount> names{
            S("tab.collection", "Collection"),
            S("tab.compatibility", "Compatibility"),
            S("tab.gpu", "GPU Selection"),
        };
        for (int index = 0; index < TabCount; ++index)
        {
            auto button = std::make_shared<SButton>();
            auto label = std::make_shared<STextBlock>();
            label->SetText(names[static_cast<std::size_t>(index)]);
            label->SetFontSize(18);
            button->SetContent(label);
            button->Padding = FMargin(16, 6);
            button->OnClicked = [this, index] { if (!busy) SelectTab(static_cast<ShellTab>(index)); };
            strip->AddSlot(button).AutoSize().SetPadding(FMargin(index == 0 ? 0.0f : 8.0f, 0, 0, 0));
            tabButtons.push_back(button);
            tabLabels.push_back(label);
        }
        strip->AddSlot(std::make_shared<SSpacer>()).Fill(1);
        // Language, kept on the right edge of the tab strip. The combo's popup is
        // driven from Mouse()/WM_KEYDOWN like the other two combos -- a popup is
        // not part of the widget tree it floats over.
        langBox = std::make_shared<SComboBox<int>>();
        langBox->Options = {static_cast<int>(ELanguage::English), static_cast<int>(ELanguage::ChineseSimplified)};
        langBox->SelectedIndex = static_cast<int32_t>(GetLanguage()) == 1 ? 1 : 0;
        langBox->SelectedItem = langBox->Options[static_cast<std::size_t>(langBox->SelectedIndex)];
        langBox->OnGetLabel = [](int language) { return LanguageName(static_cast<ELanguage>(language)); };
        langBox->OnSelectionChanged = [this](int language, int32_t) { if (!busy) ApplyLanguage(static_cast<ELanguage>(language)); };
        langBox->Size = Vector2(150, 32);
        langBox->ItemHeight = 32;
        langBox->FontSize = 17;
        langBox->MaxDropdownHeight = langBox->ItemHeight * 2;
        langBox->ExternalPopup = true;
        strip->AddSlot(langBox).AutoSize().SetPadding(FMargin(16, 0, 0, 0));
        Interactive(langBox);

        scroll = std::make_shared<SScrollBox>();
        auto scrollBar = std::make_shared<SScrollBar>();
        scroll->SetScrollBar(scrollBar);
        scrollBar->Thickness = 16;
        scrollBar->MinThumbLength = 32;
        scroll->ScrollSpeed = 40;
        auto view = std::make_shared<SScrollPanel>(scroll, scrollBar);

        auto root = std::make_shared<SVerticalBox>();
        root->AddSlot(strip).AutoSize().SetPadding(FMargin(22, 14, 22, 8));
        root->AddSlot(view).Fill(1);
        SlateApplication::Get().SetRootContent(root);

        BuildCollection();
        BuildCompatibility();
        BuildGpu();
        for (int index = 0; index < TabCount; ++index)
        {
            scroll->AddChild(tabs[static_cast<std::size_t>(index)]);
        }
        // Populate the GPU tab up front: DXGI enumeration is cheap and an empty
        // tab is worse than one that already has the answer in it.
        RefreshGpu();
        SelectTab(tab);
    }
    void SelectTab(ShellTab next)
    {
        tab = next;
        const int active = static_cast<int>(next);
        for (int index = 0; index < TabCount; ++index)
        {
            const bool isActive = index == active;
            tabs[static_cast<std::size_t>(index)]->Visibility = isActive ? EVisibility::Visible : EVisibility::Collapsed;
            tabButtons[static_cast<std::size_t>(index)]->NormalColor = isActive ? ActiveTabColor : IdleTabColor;
            tabLabels[static_cast<std::size_t>(index)]->SetColor(isActive ? Colors::White : DimColor);
        }
        if (kindBox) kindBox->ClosePopup();
        if (captureMode) captureMode->ClosePopup();
        if (langBox) langBox->ClosePopup();
        if (scroll) scroll->ScrollOffset = 0;
        ResetPointerGesture();
    }
    void BuildCollection()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::Collection)] = std::make_shared<SVerticalBox>();
        Text(S("collection.title", "Windows Environment Collection"), 29);
        Text(S("collection.project", "Project: ") + options.profile.displayName, 18);
        // Red, above everything the user fills in: the anchor decides whether
        // the package is worth anything, and it is the one thing that has been
        // forgotten three times in a row.
        anchorWarning = Text("", 16);
        anchorWarning->SetColor(FailColor);
        Text(S("collection.intro",
               "Collects hardware, BIOS, drivers, system events and existing memory test results, "
               "and attaches the newest game logs the project profile asks for."), 16);
        Text(S("collection.safety", "No uploading, no launching the game, no stress test, no reboot, no BIOS change."), 16);
        Text(S("collection.kind", "Incident type"), 16);
        kindBox = std::make_shared<SComboBox<IncidentKind>>();
        kindBox->Options = {IncidentKind::Unknown, IncidentKind::Crash, IncidentKind::Hang};
        kindBox->SelectedItem = options.incidentKind;
        kindBox->SelectedIndex = static_cast<int32_t>(options.incidentKind);
        kindBox->ExternalPopup = true;
        kindBox->OnGetLabel = [](IncidentKind kind) {
            return kind == IncidentKind::Crash ? S("collection.kind.crash", "Crash")
                 : kind == IncidentKind::Hang  ? S("collection.kind.hang", "Hang")
                                               : S("collection.kind.unknown", "Unsure");
        };
        kindBox->OnSelectionChanged = [this](IncidentKind kind, int32_t) {
            if (busy) return;
            options.incidentKind = kind;
            RefreshAnchorWarning();
        };
        kindBox->Size = Vector2(260, 32);
        kindBox->ItemHeight = 32;
        kindBox->FontSize = 18;
        kindBox->MaxDropdownHeight = kindBox->ItemHeight * 3;
        AddWidget(kindBox, 6);
        Interactive(kindBox);
        Text(S("collection.time", "When it happened (leave empty to find one automatically; include the time zone)"), 16);
        timeInput = std::make_shared<SEditableTextBox>();
        timeInput->FontSize = 18; timeInput->MinWidth = 560;
        timeInput->SetHintText("2026-10-12T18:30:00+08:00");
        timeInput->SetText(FormatUtcIso8601(options.incidentTime));
        AddWidget(timeInput, 6);
        Interactive(timeInput);
        timeLabel = Button(S("collection.time.fill", "Use current time"),
                           [this] { if (!busy) timeInput->SetText(FormatUtcIso8601(CurrentUtcTime())); });
        details = std::make_shared<SCheckBox>();
        details->SetLabel(S("collection.events", "Include event details"));
        details->SetChecked(true);
        details->FontSize = 16; details->BoxSize = 18;
        details->Size = Vector2(260, 28);
        AddWidget(details, 9);
        Interactive(details);
        Button(S("collection.start", "Start collection and build ZIP"), [this] { Start(); });
        Button(S("collection.cancel", "Cancel collection (not cancellable while packing)"),
               [this] { if (busy && job == Job::Collection && cancellable.load()) cancel = true; });
        status = Text(S("collection.ready", "Ready. Reports are saved in the EnvironmentReports folder on the desktop."), 16);
        Button(S("collection.open", "Open report folder"), [this] {
            if (busy) return;
            try
            {
                const auto path = result.archive.empty() ? (options.outputDirectory.empty() ? DefaultOutputDirectory() : options.outputDirectory) : result.archive.parent_path();
                std::filesystem::create_directories(path);
                ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            catch (...) { status->SetText(S("collection.open.failed", "Cannot open the folder; check access permissions.")); }
        });
        Text(S("collection.after", "Review the report when it is done, then send the ZIP manually."), 16);
        Text(S("collection.disclaimer",
               "No anomaly found does not mean the hardware is healthy; failed steps are recorded in the report."), 16);
        Text(std::string(S("capture.section", "Advanced diagnostics: capture one crash · ")) + zengine::envcheck::ToolVersion, 24);
        Text(S("capture.privacy",
               "A full memory dump may contain account and chat data. It stays on this machine; do not share it publicly."), 16);
        captureMode = std::make_shared<SComboBox<int>>();
        captureMode->Options = {0, 1}; captureMode->SelectedIndex = 0; captureMode->SelectedItem = 0;
        captureMode->Size = Vector2(460, 32); captureMode->FontSize = 18;
        captureMode->ItemHeight = 32; captureMode->MaxDropdownHeight = 64; captureMode->ExternalPopup = true;
        captureMode->OnGetLabel = [](int mode) {
            return mode == 0 ? S("capture.mode.wer", "Windows system crash dump (recommended)")
                             : S("capture.mode.procdump", "Attach ProcDump (needs debug permission)");
        };
        AddWidget(captureMode, 6);
        Interactive(captureMode);
        Text(S("capture.wer.note",
               "WER is configured for the current exe name only. It needs UAC, stays enabled for repeated crashes "
               "in this session, is restored only after you click 'Stop capture safely', and never attaches to the game."), 16);
        Text(S("capture.procdump.note", "The ProcDump download and selection below are for ProcDump mode only."), 16);
        Button(S("capture.procdump.download", "Open the Microsoft ProcDump download and license page"), [this] {
            if (!busy) ShellExecuteW(window, L"open", L"https://learn.microsoft.com/en-us/sysinternals/downloads/procdump", nullptr, nullptr, SW_SHOWNORMAL);
        });
        procDumpPath = LoadProcDumpPreference();
        procDumpLabel = Button(procDumpPath.empty() ? S("capture.procdump.pick", "Select procdump64.exe")
                                                    : S("capture.procdump.reselect", "Reselect ProcDump (location remembered)"), [this] {
            if (busy) return;
            auto path = ChooseExecutable(W("capture.procdump.dialog", "Select the official Microsoft procdump64.exe").c_str());
            if (!path.empty()) { procDumpPath = path; procDumpLabel->SetText(S("capture.procdump.selected", "ProcDump selected (signature verified before start)")); }
        });
        targetLabel = Button(S("capture.target.pick", "Select the game program (the current project target)"), [this] {
            if (busy) return;
            auto path = ChooseExecutable(W("capture.target.dialog", "Select the project's game exe, not the launcher").c_str());
            if (!path.empty()) { targetPath = path; targetLabel->SetText(S("capture.target.prefix", "Target: ") + WideToUtf8(path.filename().wstring())); }
        });
        Button(S("capture.start", "Start crash capture (as selected)"), [this] { StartCapture(); });
        Button(S("capture.import", "Pick an existing dump and pack it (no system change)"), [this] {
            if (busy) return;
            auto file = ChooseExecutable(W("capture.import.dialog", "Select an existing full dump").c_str(), L"Memory dump (*.dmp)\0*.dmp\0\0");
            if (!file.empty()) StartCapture(file);
        });
        Button(S("capture.stop", "Stop capture safely / cancel post-processing"),
               [this] { if (busy && (job == Job::Capture || job == Job::WerRestore) && cancellable.load()) cancel = true; });
        Button(S("capture.restore", "Restore the project's original WER settings (including an unfinished session)"), [this] { RestoreWer(); });
        captureStatus = Text(!options.profile.gameExecutable.empty() && HasWerBackup(options.profile.gameExecutable)
            ? S("capture.status.pending", "An unrestored WER session was found; restore the original settings first.")
            : S("capture.status.idle", "Not enabled. Waits up to 1 hour by default, and enabling it does not guarantee Windows will write a dump."), 16);
        Button(S("capture.open", "Open capture result (locate the ZIP or capture.json)"), [this] {
            if (busy) return;
            if (captureReport.directory.empty())
            { captureStatus->SetText(S("capture.open.empty", "This window has no capture report yet; earlier results are in crash-capture-* under EnvironmentReports.")); return; }
            const auto file = captureReport.archive.empty() ? captureReport.directory / "capture.json" : captureReport.archive;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str());
            HRESULT opened = E_FAIL;
            if (item) { opened = SHOpenFolderAndSelectItems(item, 0, nullptr, 0); ILFree(item); }
            if (SUCCEEDED(com)) CoUninitialize();
            if (FAILED(opened))
            {
                const auto handle = ShellExecuteW(window, L"open", captureReport.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(handle) <= 32) captureStatus->SetText(S("capture.open.failed", "Cannot open the capture folder; look for capture.json under EnvironmentReports/crash-capture-*."));
            }
        });
    }
    void BuildCompatibility()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::Compatibility)] = std::make_shared<SVerticalBox>();
        Text(S("check.title", "Windows Environment Compatibility Check"), 29);
        Text(S("check.intro",
               "The same pipeline as the default -batchmode command: hardware snapshot, dual-GPU selection check, "
               "static DLL checks and the isolated load probe."), 16);
        Text(S("check.output", "Report directory"), 16);
        checkOutputInput = std::make_shared<SEditableTextBox>();
        checkOutputInput->FontSize = 18; checkOutputInput->MinWidth = 560;
        checkOutputInput->SetHintText(S("check.output.hint", "Leave empty to use EnvironmentReports on the desktop"));
        checkOutputInput->SetText(WideToUtf8(DefaultOutputDirectory().wstring()));
        AddWidget(checkOutputInput, 6);
        Interactive(checkOutputInput);
        Text(S("check.isa", "Required instruction sets (none, or avx,avx2,fma,f16c)"), 16);
        checkRequireInput = std::make_shared<SEditableTextBox>();
        checkRequireInput->FontSize = 18; checkRequireInput->MinWidth = 560;
        checkRequireInput->SetHintText("none");
        checkRequireInput->SetText("none");
        AddWidget(checkRequireInput, 6);
        Interactive(checkRequireInput);
        Text(S("check.memory", "Minimum physical memory (GB)"), 16);
        checkMemoryInput = std::make_shared<SEditableTextBox>();
        checkMemoryInput->FontSize = 18; checkMemoryInput->MinWidth = 560;
        checkMemoryInput->SetHintText("4");
        checkMemoryInput->SetText("4");
        AddWidget(checkMemoryInput, 6);
        Interactive(checkMemoryInput);
        checkDllBox = std::make_shared<SCheckBox>();
        checkDllBox->SetLabel(S("check.dll", "Include recursive static DLL checks and the isolated load probe (can be slow)"));
        checkDllBox->SetChecked(true);
        checkDllBox->FontSize = 16; checkDllBox->BoxSize = 18;
        checkDllBox->Size = Vector2(560, 28);
        AddWidget(checkDllBox, 9);
        Interactive(checkDllBox);
        Button(S("check.start", "Start check and build report"), [this] { StartCompatibility(); });
        Button(S("check.cancel", "Cancel check"), [this] { if (busy && job == Job::Compatibility && cancellable.load()) cancel = true; });
        checkStatus = Text(S("check.ready", "Ready."), 16);
        checkReportBox = std::make_shared<SVerticalBox>();
        AddWidget(checkReportBox, 6);
    }
    void BuildGpu()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::GpuSelection)] = std::make_shared<SVerticalBox>();
        Text(S("gpu.title", "GPU Automatic Selection Check"), 29);
        Text(S("gpu.intro", "Byte-identical to the selection logic in GfxDeviceSetup.cpp. Links neither the engine nor the game, and never launches the game."), 16);
        Text(S("gpu.intro2", "The top half is offline regression cases; the bottom half probes this machine (switch the discrete GPU in the BIOS and click Re-probe to check again)."), 16);
        Button(S("gpu.reprobe", "Re-probe"), [this] { if (!busy) RefreshGpu(); });
        gpuStatus = Text(S("gpu.ready", "Ready."), 16);
        gpuReportBox = std::make_shared<SVerticalBox>();
        AddWidget(gpuReportBox, 6);
    }
    // Report lines are one STextBlock each: STextBlock has no wrapping, so a
    // multi-line report has to be split before it reaches the widget tree.
    static void FillReportLines(const std::shared_ptr<SVerticalBox>& box, const std::string& report)
    {
        box->ClearChildren();
        std::istringstream stream(report);
        std::string line;
        int count = 0;
        while (std::getline(stream, line))
        {
            if (++count > MaxReportLines)
            {
                auto truncated = std::make_shared<STextBlock>();
                truncated->SetText(S("report.truncated", "... report too long, display truncated; the full text is in OSGameEnvironmentReport.txt next to it."));
                truncated->SetFontSize(13); truncated->SetColor(DimColor);
                box->AddSlot(truncated).AutoSize().SetPadding(FMargin(22, 1));
                break;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto text = std::make_shared<STextBlock>();
            text->SetText(line);
            text->SetFontSize(13);
            const bool failure = line.rfind("[FAIL", 0) == 0;
            const bool success = line.rfind("[PASS", 0) == 0;
            text->SetColor(failure ? FailColor : success ? PassColor : DimColor);
            box->AddSlot(text).AutoSize().SetPadding(FMargin(22, 1));
        }
    }
    void RefreshGpu()
    {
        const std::vector<zengine::envcheck::GpuSelectionMockCaseResult> mockResults =
            zengine::envcheck::RunAllGpuSelectionMockCases();
        const auto realResult = zengine::envcheck::ProbeRealGpuSelection();
        gpuReportBox->ClearChildren();
        const auto add = [this](const std::string& text, float size, const UIColor& color)
        {
            auto widget = std::make_shared<STextBlock>();
            widget->SetText(text); widget->SetFontSize(size); widget->SetColor(color);
            gpuReportBox->AddSlot(widget).AutoSize().SetPadding(FMargin(22, 1));
        };
        add(S("gpu.section.mock", "== Offline regression cases =="), 17, Colors::White);
        for (const auto& mockResult : mockResults)
        {
            add((mockResult.passed ? "[PASS] " : "[FAIL] ") + mockResult.caseName,
                15, mockResult.passed ? PassColor : FailColor);
            for (std::size_t index = 0; index < mockResult.adapters.size(); ++index)
            {
                add(zengine::envcheck::FormatGpuAdapterLine(index, mockResult.adapters[index], true), 13, DimColor);
            }
            add("  chosen=" + std::to_string(mockResult.chosenAdapterIndex)
                + "  expected=" + std::to_string(mockResult.expectedAdapterIndex)
                + "  excludedIntegrated=" + (mockResult.excludedIntegrated ? "true" : "false"), 13, DimColor);
        }
        const bool allPassed = zengine::envcheck::AllGpuSelectionMockCasesPassed(mockResults);
        add(allPassed ? S("gpu.regression.pass", "Regression: all passed")
                      : S("gpu.regression.fail", "Regression: there are failures (the selection logic may differ from GfxDeviceSetup.cpp)"),
            16, allPassed ? PassColor : FailColor);
        add(S("gpu.section.real", "== Real probe on this machine =="), 17, Colors::White);
        add(S("gpu.all.adapters", "All adapters (EnumAdapters1):"), 14, AccentColor);
        for (std::size_t index = 0; index < realResult.allAdapters.size(); ++index)
        {
            add(zengine::envcheck::FormatGpuAdapterLine(index, realResult.allAdapters[index], false), 13, DimColor);
        }
        if (realResult.gpuPreferencePicked)
        {
            add(S("gpu.preference.picked", "EnumAdapterByGpuPreference(HIGH_PERFORMANCE) selected: ")
                + realResult.preferredAdapter.name
                + "  dedicated=" + zengine::envcheck::FormatGpuMegabytes(realResult.preferredAdapter.dedicatedVideoMemoryBytes),
                14, PassColor);
        }
        else
        {
            add(S("gpu.preference.failed", "EnumAdapterByGpuPreference unavailable or failed: ")
                + (realResult.errorMessage.empty() ? std::string(S("gpu.reason.unknown", "unknown reason")) : realResult.errorMessage),
                14, FailColor);
        }
        gpuStatus->SetText(allPassed ? S("gpu.status.pass", "Probe finished, regression all passed.")
                                     : S("gpu.status.fail", "Probe finished, regression has failures; please screenshot this page and send it to the developers."));
    }
    std::filesystem::path ChooseExecutable(const wchar_t* title, const wchar_t* filter = L"Windows program (*.exe)\0*.exe\0\0")
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = path.data(); dialog.nMaxFile = static_cast<DWORD>(path.size()); dialog.lpstrTitle = title;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
    }
    void SetBusy(Job next)
    {
        job = next;
        busy = true;
        cancel = false;
        finished = false;
        cancellable = true;
        if (kindBox) kindBox->ClosePopup();
        if (captureMode) captureMode->ClosePopup();
        if (langBox) langBox->ClosePopup();
        for (const auto& widget : interactive) widget->Visibility = EVisibility::HitTestInvisible;
        { std::lock_guard lock(mutex); progress = S("status.preparing", "Preparing"); error.clear(); }
    }
    void ClearBusy()
    {
        busy = false;
        job = Job::None;
        for (const auto& widget : interactive) widget->Visibility = EVisibility::Visible;
    }
    void StartCompatibility()
    {
        if (busy) return;
        zengine::envcheck::EnvironmentCheckOptions input;
        const std::string directory = checkOutputInput->GetText();
        input.outputDirectory = directory.empty() ? DefaultOutputDirectory() : std::filesystem::path(Utf8ToWide(directory));
        try
        {
            zengine::envcheck::ApplyRequiredFeatures(input.requirements, checkRequireInput->GetText());
        }
        catch (const std::exception&)
        {
            checkStatus->SetText(S("check.isa.invalid", "Invalid instruction set; use none or a combination of avx,avx2,fma,f16c."));
            return;
        }
        try
        {
            const std::string memory = checkMemoryInput->GetText();
            if (!memory.empty())
            {
                const unsigned long long gibibytes = std::stoull(memory);
                input.requirements.minimumMemoryBytes = gibibytes * 1024ull * 1024ull * 1024ull;
            }
        }
        catch (const std::exception&)
        {
            checkStatus->SetText(S("check.memory.invalid", "Minimum memory must be a whole number of GB."));
            return;
        }
        input.includeDllChecks = checkDllBox->IsChecked();
        input.quiet = true;   // the GUI shows the report itself
        checkOutcome = {};
        FillReportLines(checkReportBox, std::string{});
        SetBusy(Job::Compatibility);
        worker = std::thread([this, input] {
            try
            {
                auto outcome = zengine::envcheck::RunEnvironmentCheck(input, cancel, [this](const std::string& message) {
                    std::lock_guard lock(mutex); progress = message;
                });
                std::lock_guard lock(mutex); checkOutcome = std::move(outcome);
            }
            catch (const std::exception& exception) { std::lock_guard lock(mutex); error = exception.what(); }
            finished = true;
        });
    }
    void StartCapture(const std::filesystem::path& import = {})
    {
        if (busy) return;
        const bool wer = captureMode->SelectedItem == 0 || !import.empty();
        if (targetPath.empty() || (!wer && procDumpPath.empty()))
        { captureStatus->SetText(wer ? S("capture.need.target", "Select the game program first.")
                                     : S("capture.need.both", "Select ProcDump and the game program first.")); return; }
        if (options.profile.gameExecutable.empty())
        { captureStatus->SetText(S("capture.need.profile", "The current profile has no target exe; use a project profile.")); return; }
        // Consent dialogs are the one place the wording is contractual, so they
        // are whole strings per language rather than assembled fragments.
        const std::wstring notice = !import.empty()
            ? W("capture.consent.import",
                "Confirm this full dump came from the selected game. Import only checks the structure and module "
                "paths; it cannot prove the dump belongs to this run.\nIt may contain accounts, chats and tokens, "
                "uses several GB, is never uploaded, and the original file is not deleted.\n\nConfirm the source and pack it?")
            : wer ? W("capture.consent.wer.prefix", "Only the ") + Utf8ToWide(options.profile.gameExecutable)
                    + W("capture.consent.wer.suffix",
                        " key under HKLM LocalDumps is configured, DumpType=2, writing to this report's private "
                        "system-dumps folder.\nThe setting applies to every program with that exe name; it needs UAC, "
                        "and UAC is asked again to restore it when collection ends.\nOne activation covers repeated "
                        "crashes in this session (several GB each, 8 at most); it stays in force until you click "
                        "'Stop capture safely'.\nA full dump contains sensitive data, uses several GB and is never "
                        "uploaded. Configuring it does not guarantee a dump.\nIt does not attach to the game and does "
                        "not stop WER; use the restore button after an abnormal exit.\n\nDo you agree?")
            : W("capture.consent.procdump",
                "Only the selected game is monitored. A full dump contains accounts, chats and other sensitive data, "
                "uses several GB and is never uploaded.\nOn stop, ProcDump is waited for and neither the game nor the "
                "debugger is killed.\n\nDo you agree?");
        if (MessageBoxW(window, notice.c_str(),
            W("capture.consent.title", "Full memory capture authorisation").c_str(),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        if (!wer && MessageBoxW(window,
            W("capture.license.body",
              "Please read the Microsoft Sysinternals license first (the download page and the Eula.txt shipped with ProcDump).\n"
              "Choosing Yes means you have read and accepted it; the collector will call the ProcDump you selected with -accepteula.\n"
              "This tool does not contain ProcDump and does not obtain that third-party license for you.\n\nAccept the license?").c_str(),
            W("capture.license.title", "Sysinternals license").c_str(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        CaptureOptions input;
        input.environment = options; input.environment.includeEvents = details->IsChecked();
        input.procDump = procDumpPath; input.gameExecutable = targetPath;
        input.memoryConsent = true; input.licenseAccepted = !wer;
        WerOptions system; system.environment = input.environment; system.gameExecutable = targetPath;
        system.memoryConsent = true; system.importDump = import; system.importConfirmed = !import.empty(); system.owner = window;
        SetBusy(Job::Capture);
        { std::lock_guard lock(mutex); error.clear(); captureMessage.clear(); captureReport = {}; progress = S("capture.preparing", "Preparing capture"); }
        worker = std::thread([this, input, system, wer] {
            try
            {
                auto update = [this](const std::string& message, bool canCancel) {
                    cancellable = canCancel; std::lock_guard lock(mutex); progress = message;
                };
                auto captured = wer ? CaptureWer(system, cancel, update) : CaptureCrash(input, cancel, update);
                const bool remember = captured.procDumpVerified;
                { std::lock_guard lock(mutex); captureReport = std::move(captured.report); captureMessage = captured.message; }
                if (remember)
                {
                    try { SaveProcDumpPreference(input.procDump); }
                    catch (...) { std::lock_guard lock(mutex); captureMessage += S("capture.forget.failed", " (could not remember the ProcDump location; the evidence is unaffected)"); }
                }
            }
            catch (const std::exception& exception) { std::lock_guard lock(mutex); error = exception.what(); }
            finished = true;
        });
    }
    void RestoreWer()
    {
        if (busy || options.profile.gameExecutable.empty()) return;
        if (MessageBoxW(window,
            W("capture.restore.dialog.body",
              "Restore the WER backup of the project's exe; UAC may be required.\nOnly the values this collector saved "
              "are restored, no dump is deleted, and it refuses to overwrite if another tool changed them.\n\nContinue?").c_str(),
            W("capture.restore.dialog.title", "Restore system capture settings").c_str(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        SetBusy(Job::WerRestore);
        cancellable = false;
        { std::lock_guard lock(mutex); error.clear(); captureMessage.clear(); progress = S("capture.restore.progress", "Waiting for restore authorisation"); }
        worker = std::thread([this] {
            DWORD code = RequestWerRestore(options.profile.gameExecutable, window);
            { std::lock_guard lock(mutex); captureMessage = code == ERROR_SUCCESS ? S("capture.restore.ok", "WER settings restored (or there was nothing to restore); existing dumps are kept.")
                : S("capture.restore.failed.prefix", "Restore did not finish: Win32=") + std::to_string(code) + S("capture.restore.failed.suffix", ". UAC may have been cancelled or the settings conflict; the backup is kept."); }
            finished = true;
        });
    }
    bool ConfirmClose()
    {
        if (!options.profile.gameExecutable.empty() && HasWerBackup(options.profile.gameExecutable))
            return MessageBoxW(window,
                W("capture.close.body",
                  "A WER backup is still unrestored, so later crashes of a program with the same name may keep "
                  "producing dumps.\nBetter go back and click restore. Exit anyway?").c_str(),
                W("capture.close.title", "System capture settings not restored").c_str(),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        return true;
    }
    // A package missing any of the three anchor inputs cannot answer the one
    // question it was collected for -- "was that the game?" -- and the discovery
    // used to happen on the developer's side, days later. Surface it on the
    // form, and again before the ZIP is built.
    void RefreshAnchorWarning()
    {
        if (!anchorWarning) return;
        Options current = options;
        if (timeInput)
        {
            const auto text = timeInput->GetText();
            current.incidentTime = 0;
            // An unparseable time is "not supplied" as far as attribution is
            // concerned; Start() reports the format error separately.
            if (!text.empty() && !TryParseUtcIso8601(text, current.incidentTime)) current.incidentTime = 0;
        }
        const auto anchor = DescribeAttributionAnchor(current);
        if (anchor.Usable()) { anchorWarning->SetText({}); return; }
        std::vector<std::string> missing;
        if (!anchor.incidentTime) missing.push_back(S("collection.anchor.missing.time", "when it happened"));
        if (!anchor.incidentKind) missing.push_back(S("collection.anchor.missing.kind", "what happened"));
        if (!anchor.gameExecutable) missing.push_back(S("collection.anchor.missing.game", "the game exe (none in this profile)"));
        std::string text = S("collection.anchor.warning",
                             "This package cannot be used to attribute an incident without: ");
        for (std::size_t index = 0; index < missing.size(); ++index)
        {
            if (index) text += ", ";
            text += missing[index];
        }
        anchorWarning->SetText(text);
    }
    void Start()
    {
        if (busy) return;
        Options input = options;
        const auto time = timeInput->GetText();
        input.incidentTime = 0;
        if (!time.empty() && !TryParseUtcIso8601(time, input.incidentTime))
        { status->SetText(S("status.time.invalid", "Invalid time format; use an ISO time with Z or +08:00.")); return; }
        if (input.incidentTime > CurrentUtcTime() + 5 * TicksPerMinute)
        { status->SetText(S("status.time.future", "The incident time cannot be in the future.")); return; }
        input.includeEvents = details->IsChecked();
        // Deliberately a confirmation, not a block: sweeping a machine's health
        // with no incident to anchor to is a legitimate use. But the user must
        // know, before the ZIP exists, what they will be handing over.
        if (!DescribeAttributionAnchor(input).Usable()
            && MessageBoxW(window,
                W("collection.confirm.anchor.body",
                  "This collection has no incident time, no incident type, or no game exe, so the package "
                  "cannot say whether the game caused anything -- only what this machine looks like.\n"
                  "If you are sending this to a developer for a crash or a freeze, go back and fill them in first.\n\n"
                  "Collect an unattributable package anyway?").c_str(),
                W("collection.confirm.anchor.title", "No incident anchor").c_str(),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        SetBusy(Job::Collection);
        worker = std::thread([this, input] {
            try
            {
                auto collected = Collect(input, cancel, [this](const std::string& message, bool canCancel) {
                    cancellable = canCancel;
                    std::lock_guard lock(mutex); progress = message;
                });
                std::lock_guard lock(mutex); result = std::move(collected);
            }
            catch (const std::exception&) { std::lock_guard lock(mutex); error = S("status.collect.failed", "Collection or packing failed; files already written are kept. Check the output directory."); }
            finished = true;
        });
    }
    void Update()
    {
        if (!busy) return;
        if (closing && cancellable.load()) cancel = true;
        {
            std::lock_guard lock(mutex);
            status->SetText(progress + " …");
            if (job == Job::Capture || job == Job::WerRestore) captureStatus->SetText(progress + " …");
            if (job == Job::Compatibility) checkStatus->SetText(progress + " …");
        }
        if (finished.load())
        {
            worker.join();
            const Job completed = job;
            ClearBusy();
            std::lock_guard lock(mutex);
            // A failed job is the reason someone sends us the log, so it goes in
            // there as well as on the status line.
            if (!error.empty()) zengine::envcheck::LogError(std::string(JobName(completed)) + " failed: " + error);
            if (completed == Job::Compatibility)
            {
                FillReportLines(checkReportBox, checkOutcome.textReport);
                checkStatus->SetText(error.empty()
                    ? S("check.done.prefix", "Check finished, exit code ") + std::to_string(checkOutcome.exitCode)
                        + S("check.done.suffix", ". Report: ") + checkOutcome.textPath.string()
                    : error);
            }
            else if (completed == Job::Capture || completed == Job::WerRestore)
            {
                captureStatus->SetText(error.empty() ? captureMessage : error);
                status->SetText(error.empty() ? captureMessage : error);
            }
            else status->SetText(!error.empty() ? error : result.cancelled ? S("status.cancelled", "Cancelled; a ZIP with partial information was produced.")
                : result.partial ? S("status.partial", "ZIP built; some steps failed, see the manifest.")
                                 : S("status.done", "ZIP built. Review the report, then send it manually."));
            if (closing) { closing = false; if (ConfirmClose()) DestroyWindow(window); }
        }
    }
    void Mouse(float wheel = 0)
    {
        if (suppressPointerUntilRelease)
        {
            if (!platform.down) suppressPointerUntilRelease = false;
            return;
        }
        // The language combo sits on the tab strip, so it is live on every tab --
        // it is handled before the per-tab combos, which are inside the scroll
        // area and only exist on the collection tab.
        if (langBox && langBox->ProcessPopupMouse(platform.mouse, platform.down, wheel))
        {
            if (!langBox->IsOpen()) ResetPointerGesture();
            return;
        }
        if (tab == ShellTab::Collection)
        {
            if (kindBox && kindBox->ProcessPopupMouse(platform.mouse, platform.down, wheel))
            {
                if (!kindBox->IsOpen()) ResetPointerGesture();
                return;
            }
            if (captureMode && captureMode->ProcessPopupMouse(platform.mouse, platform.down, wheel))
            {
                if (!captureMode->IsOpen()) ResetPointerGesture();
                return;
            }
        }
        router.ProcessMouse(SlateApplication::Get().GetRootContent(), platform.mouse,
            platform.mouse.x >= 0 && platform.mouse.y >= 0 && platform.mouse.x < platform.size.x && platform.mouse.y < platform.size.y,
            platform.down, wheel);
    }
};
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto* app = reinterpret_cast<ShellApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<ShellApp*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, w, l);
    switch (message)
    {
    case WM_SIZE:
        app->minimized = w == SIZE_MINIMIZED;
        if (!app->minimized && LOWORD(l) && HIWORD(l))
        {
            // Swapchain stays physical; the widget tree gets the logical size.
            app->SetPhysicalSize(LOWORD(l), HIWORD(l));
            if (app->backend) app->backend->Resize(LOWORD(l), HIWORD(l));
        }
        return 0;
    case WM_DPICHANGED:
    {
        app->ApplyDpi(HIWORD(w));
        const auto* rect = reinterpret_cast<const RECT*>(l);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_MOUSEMOVE:
        app->platform.mouse = app->Logical(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        app->Mouse(); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        app->platform.mouse = app->Logical(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        app->platform.down = message == WM_LBUTTONDOWN;
        if (app->platform.down) SetCapture(window);
        app->Mouse();
        if (!app->platform.down) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (!app->platform.down) return 0;
        [[fallthrough]];
    case WM_KILLFOCUS:
        app->platform.down = false;
        if (app->kindBox) app->kindBox->ClosePopup();
        if (app->captureMode) app->captureMode->ClosePopup();
        if (app->langBox) app->langBox->ClosePopup();
        app->router.Reset(); return 0;
    case WM_MOUSEWHEEL:
    {
        POINT pos{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(window, &pos);
        app->platform.mouse = app->Logical(pos.x, pos.y);
        app->Mouse(static_cast<float>(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA); return 0;
    }
    case WM_KEYDOWN:
    {
        const EKey key = w == VK_UP ? EKey::Up : w == VK_DOWN ? EKey::Down : w == VK_RETURN ? EKey::Enter
            : w == VK_ESCAPE ? EKey::Escape : w == VK_SPACE ? EKey::Space : w == VK_TAB ? EKey::Tab : EKey::Unknown;
        if (app->langBox && app->langBox->IsOpen())
        {
            app->langBox->OnKeyDown(key);
            if (!app->langBox->IsOpen()) app->ResetPointerGesture();
        }
        else if (app->tab == ShellTab::Collection)
        {
            if (app->kindBox && app->kindBox->IsOpen())
            {
                app->kindBox->OnKeyDown(key);
                if (!app->kindBox->IsOpen()) app->ResetPointerGesture();
            }
            else if (app->captureMode && app->captureMode->IsOpen())
            {
                app->captureMode->OnKeyDown(key);
                if (!app->captureMode->IsOpen()) app->ResetPointerGesture();
            }
            else if (!app->busy) app->router.ProcessKey(key);
        }
        else if (!app->busy)
        {
            app->router.ProcessKey(key);
        }
        return 0;
    }
    case WM_CHAR:
        if (!app->busy && w < 128 && (!app->kindBox || !app->kindBox->IsOpen())
            && (!app->captureMode || !app->captureMode->IsOpen()) && (!app->langBox || !app->langBox->IsOpen()))
            app->router.ProcessChar(static_cast<unsigned>(w));
        return 0;
    case WM_CLOSE:
        if (app->busy) { app->closing = true; if (app->cancellable.load()) app->cancel = true; return 0; }
        if (app->ConfirmClose()) DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, w, l);
    }
}
} // namespace

int RunEnvironmentCheckGui(int show, int argumentCount, wchar_t** arguments)
{
    // No FreeConsole() here any more: as a WINDOWS-subsystem binary there is no
    // console to drop, which is what stops the double-click flash.
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    try
    {
        ShellApp app;
        bool languageFromCommandLine = false;
        auto defaultProfile = ExecutableDirectory() / L"profiles" / L"default.ini";
        if (std::filesystem::is_regular_file(defaultProfile)) app.options.profile = LoadProfile(defaultProfile);
        for (int i = 1; i < argumentCount; ++i)
        {
            auto value = [&]() -> std::wstring { if (++i >= argumentCount) throw std::invalid_argument("Missing value"); return arguments[i]; };
            std::wstring key = arguments[i];
            if (key == L"--profile") app.options.profile = LoadProfile(value());
            else if (key == L"--output-dir") app.options.outputDirectory = value();
            else if (key == L"--incident-time") { if (!TryParseUtcIso8601(WideToUtf8(value()), app.options.incidentTime)) throw std::invalid_argument("Invalid incident time"); }
            else if (key == L"--tab")
            {
                const std::wstring name = value();
                if (name == L"collection") app.tab = ShellTab::Collection;
                else if (name == L"compatibility" || name == L"check") app.tab = ShellTab::Compatibility;
                else if (name == L"gpu") app.tab = ShellTab::GpuSelection;
                else throw std::invalid_argument("Unknown tab; use collection|compatibility|gpu");
            }
            else if (key == L"--lang")
            {
                // Wins over the remembered choice, so a screenshot session or a
                // support walkthrough can force a language without touching the
                // registry.
                ZSlate::ELanguage language{};
                if (!ZSlate::LanguageFromId(WideToUtf8(value()).c_str(), language))
                    throw std::invalid_argument("Unknown language; use en|zh-CN");
                ZSlate::SetLanguage(language);
                languageFromCommandLine = true;
            }
            else throw std::invalid_argument("Unsupported GUI option. CLI commands need -batchmode; "
                                             "see ZEnvironmentCollector.exe -batchmode --help");
        }
        // UI language: default English, remembered per user from the last visit.
        // Registration happens before Build() so every string the tree asks for
        // is already there. The table is copied across because UiLocalization.cpp
        // is deliberately ZSlate-free (the CLI-only build links it too), so it
        // cannot hand out ZSlate's own entry type.
        {
            std::size_t count = 0;
            const zengine::envcheck::ui::UiStringEntry* table = zengine::envcheck::ui::ChineseSimplifiedStrings(count);
            std::vector<ZSlate::FLocalizationEntry> entries;
            entries.reserve(count);
            for (std::size_t index = 0; index < count; ++index) entries.push_back({table[index].id, table[index].text});
            ZSlate::RegisterStrings(ZSlate::ELanguage::ChineseSimplified, entries.data(), entries.size());
            ZSlate::ELanguage remembered{};
            if (!languageFromCommandLine
                && ZSlate::LanguageFromId(zengine::envcheck::ui::LoadLanguagePreference().c_str(), remembered))
                ZSlate::SetLanguage(remembered);
        }
        // ZSlate's own diagnostics (font atlas misses, the "no CJK face" warning)
        // go through 扁鹊 from here on: as a WINDOWS-subsystem binary this process
        // has no stderr anyone can read.
        ZSlate::SetLogSink([](ZSlate::ELogLevel level, const char* message) {
            if (level == ZSlate::ELogLevel::Error) zengine::envcheck::LogError(message);
            else if (level == ZSlate::ELogLevel::Warning) zengine::envcheck::LogWarning(message);
            else zengine::envcheck::LogInfo(message);
        });
        SetPlatform(&app.platform);
        if (!app.InitFonts())
        {
            zengine::envcheck::LogError("Required system fonts unavailable");
            throw std::runtime_error("Required system fonts unavailable");
        }
        app.Build();
        // WNDCLASSEX, not WNDCLASS: only the EX class carries hIconSm, and
        // hIconSm is what the taskbar shows. With the plain class the .rc that
        // z_embed_app_icon() attaches would give Explorer an icon while the
        // taskbar button kept the system default one.
        WNDCLASSEXW klass{};
        klass.cbSize        = sizeof(klass);
        klass.lpfnWndProc   = WindowProc;
        klass.hInstance     = instance;
        klass.lpszClassName = L"ZEnvironmentCollector";
        klass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
#ifdef IDI_ICON1
        // An explicit size makes LoadImage pick the closest of the .ico's
        // embedded sizes instead of stretching one of them.
        klass.hIcon   = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ICON1),
                                                      IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        klass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_ICON1),
                                                      IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
#endif
        if (!RegisterClassExW(&klass)) throw std::runtime_error("Window registration failed");
        HWND window = CreateWindowExW(0, klass.lpszClassName, L"ZEnvironmentCollector", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 790, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Window creation failed");
        // Title in the active language (English until the user picks otherwise).
        app.UpdateWindowTitle();
        // Window sizing at the monitor's scale (960x740 logical) is
        // Win32::ResizeWindowForDpi, shared with the other ZSlate tools.
        const UINT dpi = static_cast<UINT>(Win32::QueryDpiScaleForWindow(window) * 96.0f);
        app.ApplyDpi(dpi);
        Win32::ResizeWindowForDpi(window, 960, 740, dpi);
        auto backend = std::make_unique<ZSlateD3D11Renderer>(window);
        if (!backend->Init())
        {
            zengine::envcheck::LogError("D3D11 renderer init failed; falling back to -batchmode");
            MessageBoxW(window,
                Utf8ToWide(ZSlate::Loc("dialog.gpuinit.body", "The GUI could not start. Run ZEnvironmentCollector.exe -batchmode --collect in the same folder to produce a report.")).c_str(),
                Utf8ToWide(ZSlate::Loc("dialog.gpuinit.title", "Graphics initialisation failed")).c_str(), MB_OK | MB_ICONWARNING);
            DestroyWindow(window); return 30;
        }
        app.backend = std::move(backend);
        ShowWindow(window, show);
        bool running = true;
        while (running)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (!running) break;
            app.Update();
            if (!app.minimized && IsWindow(window))
            {
                // Paint in logical units through the DPI decorator; the batched
                // renderer underneath accumulates physical-pixel geometry.
                auto& scaled = app.platform.scaled;
                scaled.BeginFrame();
                const UIRect viewport(0, 0, app.platform.size.x, app.platform.size.y);
                SlateApplication::Get().PaintInto(&scaled, viewport);
                if (app.langBox) app.langBox->PaintPopup(&scaled, viewport);
                if (app.tab == ShellTab::Collection)
                {
                    app.kindBox->PaintPopup(&scaled, viewport);
                    app.captureMode->PaintPopup(&scaled, viewport);
                }
                scaled.EndFrame();
                app.backend->Render(app.platform.renderer, &app.fonts);
            }
            Sleep(app.minimized ? 50 : 10);
        }
        return 0;
    }
    catch (const std::exception& exception)
    {
        zengine::envcheck::LogError(std::string("GUI startup failed: ") + exception.what());
        MessageBoxW(nullptr,
            Utf8ToWide(ZSlate::Loc("dialog.startup.body", "Cannot start the collection window. Check the project profile, the system fonts and that the tool package is complete. -batchmode --collect is also available.")).c_str(),
            Utf8ToWide(ZSlate::Loc("dialog.startup.title", "Environment Collection")).c_str(), MB_OK | MB_ICONERROR);
        return 30;
    }
}
