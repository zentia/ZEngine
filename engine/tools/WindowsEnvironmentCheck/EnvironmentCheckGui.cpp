// ============================================================================
// ZSlate front-end of ZEnvironmentCollector -- three tabs over the one core.
// ============================================================================
//   tab 0  环境采集      -- safe collection + crash capture (was the collector GUI)
//   tab 1  兼容性检查    -- the old CLI's plain check, run through the same
//                          EnvironmentCheckCommand pipeline that -batchmode uses
//   tab 2  GPU 选卡验证  -- the old ZGpuSelectionInspector
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
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
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
        platform.dpiScale = static_cast<float>(dpi ? dpi : 96) / 96;
        platform.scaled.SetScale(platform.dpiScale);
        RECT client{};
        if (window && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
            SetPhysicalSize(client.right, client.bottom);
        if (kindBox) kindBox->ClosePopup();
        if (captureMode) captureMode->ClosePopup();
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
    void Build()
    {
        auto strip = std::make_shared<SHorizontalBox>();
        const std::array<const char*, TabCount> names{"环境采集", "兼容性检查", "GPU 选卡验证"};
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
        if (scroll) scroll->ScrollOffset = 0;
        ResetPointerGesture();
    }
    void BuildCollection()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::Collection)] = std::make_shared<SVerticalBox>();
        Text("Windows 环境信息采集", 29);
        Text("项目：" + options.profile.displayName, 18);
        Text("采集硬件、BIOS、驱动、系统事件、已有内存检测结果，并按项目配置附带最新游戏日志。", 16);
        Text("不上传、不启动游戏、不压测、不重启、不修改BIOS。", 16);
        Text("问题类型", 16);
        kindBox = std::make_shared<SComboBox<IncidentKind>>();
        kindBox->Options = {IncidentKind::Unknown, IncidentKind::Crash, IncidentKind::Hang};
        kindBox->SelectedItem = options.incidentKind;
        kindBox->SelectedIndex = static_cast<int32_t>(options.incidentKind);
        kindBox->ExternalPopup = true;
        kindBox->OnGetLabel = [](IncidentKind kind) {
            return kind == IncidentKind::Crash ? "闪退" : kind == IncidentKind::Hang ? "卡住" : "不确定";
        };
        kindBox->OnSelectionChanged = [this](IncidentKind kind, int32_t) { if (!busy) options.incidentKind = kind; };
        kindBox->Size = Vector2(260, 32);
        kindBox->ItemHeight = 32;
        kindBox->FontSize = 18;
        kindBox->MaxDropdownHeight = kindBox->ItemHeight * 3;
        AddWidget(kindBox, 6);
        Interactive(kindBox);
        Text("发生时间（可留空自动找记录；输入需带时区）", 16);
        timeInput = std::make_shared<SEditableTextBox>();
        timeInput->FontSize = 18; timeInput->MinWidth = 560;
        timeInput->SetHintText("2026-10-12T18:30:00+08:00");
        timeInput->SetText(FormatUtcIso8601(options.incidentTime));
        AddWidget(timeInput, 6);
        Interactive(timeInput);
        timeLabel = Button("填写当前时间", [this] { if (!busy) timeInput->SetText(FormatUtcIso8601(CurrentUtcTime())); });
        details = std::make_shared<SCheckBox>();
        details->SetLabel("同意附带事件明细");
        details->SetChecked(true);
        details->FontSize = 16; details->BoxSize = 18;
        details->Size = Vector2(260, 28);
        AddWidget(details, 9);
        Interactive(details);
        Button("开始采集并生成 ZIP", [this] { Start(); });
        Button("取消采集（打包阶段不可取消）", [this] { if (busy && job == Job::Collection && cancellable.load()) cancel = true; });
        status = Text("就绪。报告保存在桌面的 EnvironmentReports 文件夹。", 16);
        Button("打开报告所在文件夹", [this] {
            if (busy) return;
            try
            {
                const auto path = result.archive.empty() ? (options.outputDirectory.empty() ? DefaultOutputDirectory() : options.outputDirectory) : result.archive.parent_path();
                std::filesystem::create_directories(path);
                ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            catch (...) { status->SetText("无法打开目录，请检查访问权限。"); }
        });
        Text("完成后确认报告内容，将 ZIP 手动发给研发。", 16);
        Text("未发现异常不代表硬件健康；部分采集失败会记入报告。", 16);
        Text(std::string("高级诊断：捕获一次游戏崩溃 · ") + zengine::envcheck::ToolVersion, 24);
        Text("完整内存可能包含账号和聊天信息；仅本地保存，请勿公开分享。", 16);
        captureMode = std::make_shared<SComboBox<int>>();
        captureMode->Options = {0, 1}; captureMode->SelectedIndex = 0; captureMode->SelectedItem = 0;
        captureMode->Size = Vector2(460, 32); captureMode->FontSize = 18;
        captureMode->ItemHeight = 32; captureMode->MaxDropdownHeight = 64; captureMode->ExternalPopup = true;
        captureMode->OnGetLabel = [](int mode) { return mode == 0 ? "Windows系统崩溃转储（推荐）" : "ProcDump附加（需允许调试）"; };
        AddWidget(captureMode, 6);
        Interactive(captureMode);
        Text("WER仅配置当前exe名称；需要UAC，一次启用可连续收集多次崩溃，点“安全停止捕获”后才恢复，不附加游戏。", 16);
        Text("下方ProcDump下载与选择只用于ProcDump模式。", 16);
        Button("打开微软ProcDump下载及许可页面", [this] {
            if (!busy) ShellExecuteW(window, L"open", L"https://learn.microsoft.com/en-us/sysinternals/downloads/procdump", nullptr, nullptr, SW_SHOWNORMAL);
        });
        procDumpPath = LoadProcDumpPreference();
        procDumpLabel = Button(procDumpPath.empty() ? "选择 procdump64.exe" : "重新选择 ProcDump（已记住位置）", [this] {
            if (busy) return;
            auto path = ChooseExecutable(L"选择微软官方 procdump64.exe");
            if (!path.empty()) { procDumpPath = path; procDumpLabel->SetText("ProcDump已选择（启动前验证签名）"); }
        });
        targetLabel = Button("选择游戏程序（当前项目目标）", [this] {
            if (busy) return;
            auto path = ChooseExecutable(L"选择当前项目的游戏exe，不是启动器");
            if (!path.empty()) { targetPath = path; targetLabel->SetText("目标：" + WideToUtf8(path.filename().wstring())); }
        });
        Button("开始捕获崩溃（按所选方式）", [this] { StartCapture(); });
        Button("选择已有dump并打包（不改系统配置）", [this] {
            if (busy) return;
            auto file = ChooseExecutable(L"选择已生成的完整dump", L"Memory dump (*.dmp)\0*.dmp\0\0");
            if (!file.empty()) StartCapture(file);
        });
        Button("安全停止捕获 / 取消后处理", [this] { if (busy && (job == Job::Capture || job == Job::WerRestore) && cancellable.load()) cancel = true; });
        Button("恢复当前项目WER原配置（含上次未完成会话）", [this] { RestoreWer(); });
        captureStatus = Text(!options.profile.gameExecutable.empty() && HasWerBackup(options.profile.gameExecutable)
            ? "发现未恢复的WER会话，请先点击恢复原配置。" : "未开启；默认等待最多1小时，启用成功不保证系统一定产生dump。", 16);
        Button("打开捕获结果（定位ZIP或capture.json）", [this] {
            if (busy) return;
            if (captureReport.directory.empty())
            { captureStatus->SetText("本窗口还没有捕获报告；历史结果请到EnvironmentReports内的crash-capture-*目录查找。"); return; }
            const auto file = captureReport.archive.empty() ? captureReport.directory / "capture.json" : captureReport.archive;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str());
            HRESULT opened = E_FAIL;
            if (item) { opened = SHOpenFolderAndSelectItems(item, 0, nullptr, 0); ILFree(item); }
            if (SUCCEEDED(com)) CoUninitialize();
            if (FAILED(opened))
            {
                const auto handle = ShellExecuteW(window, L"open", captureReport.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(handle) <= 32) captureStatus->SetText("无法打开捕获目录，请到EnvironmentReports/crash-capture-*查找capture.json。");
            }
        });
    }
    void BuildCompatibility()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::Compatibility)] = std::make_shared<SVerticalBox>();
        Text("Windows 环境兼容性检查", 29);
        Text("与 -batchmode 默认命令同一条流水线：硬件快照、双显卡选卡验证、DLL静态检查与隔离加载探针。", 16);
        Text("报告目录", 16);
        checkOutputInput = std::make_shared<SEditableTextBox>();
        checkOutputInput->FontSize = 18; checkOutputInput->MinWidth = 560;
        checkOutputInput->SetHintText("留空则使用桌面的 EnvironmentReports");
        checkOutputInput->SetText(WideToUtf8(DefaultOutputDirectory().wstring()));
        AddWidget(checkOutputInput, 6);
        Interactive(checkOutputInput);
        Text("产品最低指令集（none 或 avx,avx2,fma,f16c）", 16);
        checkRequireInput = std::make_shared<SEditableTextBox>();
        checkRequireInput->FontSize = 18; checkRequireInput->MinWidth = 560;
        checkRequireInput->SetHintText("none");
        checkRequireInput->SetText("none");
        AddWidget(checkRequireInput, 6);
        Interactive(checkRequireInput);
        Text("最低物理内存（GB）", 16);
        checkMemoryInput = std::make_shared<SEditableTextBox>();
        checkMemoryInput->FontSize = 18; checkMemoryInput->MinWidth = 560;
        checkMemoryInput->SetHintText("4");
        checkMemoryInput->SetText("4");
        AddWidget(checkMemoryInput, 6);
        Interactive(checkMemoryInput);
        checkDllBox = std::make_shared<SCheckBox>();
        checkDllBox->SetLabel("包含递归DLL静态检查与隔离加载探针（可能较慢）");
        checkDllBox->SetChecked(true);
        checkDllBox->FontSize = 16; checkDllBox->BoxSize = 18;
        checkDllBox->Size = Vector2(560, 28);
        AddWidget(checkDllBox, 9);
        Interactive(checkDllBox);
        Button("开始检查并生成报告", [this] { StartCompatibility(); });
        Button("取消检查", [this] { if (busy && job == Job::Compatibility && cancellable.load()) cancel = true; });
        checkStatus = Text("就绪。", 16);
        checkReportBox = std::make_shared<SVerticalBox>();
        AddWidget(checkReportBox, 6);
    }
    void BuildGpu()
    {
        column = tabs[static_cast<std::size_t>(ShellTab::GpuSelection)] = std::make_shared<SVerticalBox>();
        Text("GPU 自动选卡验证", 29);
        Text("与 GfxDeviceSetup.cpp 的选卡逻辑逐字节一致；不链接引擎或游戏，不启动游戏。", 16);
        Text("上半部分是离线回归用例，下半部分是本机真实探测（改BIOS开关独显后点重新探测可复查）。", 16);
        Button("重新探测", [this] { if (!busy) RefreshGpu(); });
        gpuStatus = Text("就绪。", 16);
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
                truncated->SetText("… 报告过长，已截断显示；完整内容见同目录 OSGameEnvironmentReport.txt。");
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
        add("== 离线回归用例 ==", 17, Colors::White);
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
        add(allPassed ? "回归：全部通过" : "回归：存在失败（选卡逻辑可能与 GfxDeviceSetup.cpp 不一致）",
            16, allPassed ? PassColor : FailColor);
        add("== 本机真实探测 ==", 17, Colors::White);
        add("所有适配器（EnumAdapters1）：", 14, AccentColor);
        for (std::size_t index = 0; index < realResult.allAdapters.size(); ++index)
        {
            add(zengine::envcheck::FormatGpuAdapterLine(index, realResult.allAdapters[index], false), 13, DimColor);
        }
        if (realResult.gpuPreferencePicked)
        {
            add("EnumAdapterByGpuPreference(HIGH_PERFORMANCE) 选中："
                + realResult.preferredAdapter.name
                + "  dedicated=" + zengine::envcheck::FormatGpuMegabytes(realResult.preferredAdapter.dedicatedVideoMemoryBytes),
                14, PassColor);
        }
        else
        {
            add("EnumAdapterByGpuPreference 不可用或失败："
                + (realResult.errorMessage.empty() ? std::string("未知原因") : realResult.errorMessage),
                14, FailColor);
        }
        gpuStatus->SetText(allPassed ? "探测完成，回归全部通过。" : "探测完成，回归存在失败，请把本页截图发给研发。");
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
        for (const auto& widget : interactive) widget->Visibility = EVisibility::HitTestInvisible;
        { std::lock_guard lock(mutex); progress = "准备中"; error.clear(); }
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
            checkStatus->SetText("指令集填写不正确，只能填 none 或 avx,avx2,fma,f16c 的组合。");
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
            checkStatus->SetText("最低内存必须是整数GB。");
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
        { captureStatus->SetText(wer ? "请先选择游戏程序。" : "请先选择ProcDump和游戏程序。"); return; }
        if (options.profile.gameExecutable.empty())
        { captureStatus->SetText("当前profile没有目标exe，请使用具体项目配置。"); return; }
        const std::wstring notice = !import.empty()
            ? L"确认这个完整dump来自所选游戏。导入只验证结构及模块路径，不能证明它属于本次运行。\n可能包含账号、聊天和令牌，占用数GB空间，不自动上传，不删除原文件。\n\n是否确认来源并同意打包？"
            : wer ? L"将仅配置HKLM LocalDumps下的 " + Utf8ToWide(options.profile.gameExecutable) +
                L"，DumpType=2，输出到本次报告的system-dumps私有目录。\n配置按exe名称生效，可能影响其它同名程序；需要UAC授权，结束收集时再次请求UAC恢复原设置。\n一次启用可连续收集本次会话内的多次崩溃（每份数GB，最多8份）；点击“安全停止捕获”后才结束并统一恢复配置，未点击前配置一直生效。\n完整内存含敏感数据，占用数GB，不自动上传。配置成功不保证产生dump。\n不附加游戏，不终止WER；异常退出可用恢复按钮处理。\n\n是否同意？"
            : L"仅监控所选游戏。完整内存含账号、聊天等敏感数据，占用数GB，不自动上传。\n停止时等待ProcDump安全退出，不强杀游戏或调试器。\n\n是否同意？";
        if (MessageBoxW(window, notice.c_str(), L"完整内存采集授权", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        if (!wer && MessageBoxW(window,
            L"请先阅读微软Sysinternals许可（下载页及ProcDump自带Eula.txt）。\n"
            L"选择“是”表示你已阅读并接受，采集器将以 -accepteula 参数调用你选择的ProcDump。\n"
            L"工具不包含ProcDump，也不代替你取得第三方使用许可。\n\n是否接受该许可？",
            L"Sysinternals许可确认", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        CaptureOptions input;
        input.environment = options; input.environment.includeEvents = details->IsChecked();
        input.procDump = procDumpPath; input.gameExecutable = targetPath;
        input.memoryConsent = true; input.licenseAccepted = !wer;
        WerOptions system; system.environment = input.environment; system.gameExecutable = targetPath;
        system.memoryConsent = true; system.importDump = import; system.importConfirmed = !import.empty(); system.owner = window;
        SetBusy(Job::Capture);
        { std::lock_guard lock(mutex); error.clear(); captureMessage.clear(); captureReport = {}; progress = "准备捕获"; }
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
                    catch (...) { std::lock_guard lock(mutex); captureMessage += "（未能记住ProcDump位置，不影响取证结果）"; }
                }
            }
            catch (const std::exception& exception) { std::lock_guard lock(mutex); error = exception.what(); }
            finished = true;
        });
    }
    void RestoreWer()
    {
        if (busy || options.profile.gameExecutable.empty()) return;
        if (MessageBoxW(window, L"恢复当前项目exe的WER备份，可能需要UAC授权。\n只恢复采集器保存的原值，不删除dump；检测到其他工具修改时拒绝覆盖。\n\n继续？", L"恢复系统捕获配置", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        SetBusy(Job::WerRestore);
        cancellable = false;
        { std::lock_guard lock(mutex); error.clear(); captureMessage.clear(); progress = "等待恢复配置授权"; }
        worker = std::thread([this] {
            DWORD code = RequestWerRestore(options.profile.gameExecutable, window);
            { std::lock_guard lock(mutex); captureMessage = code == ERROR_SUCCESS ? "WER配置已恢复（或没有待恢复备份），原dump保留。"
                : "恢复未完成：Win32=" + std::to_string(code) + "。可能UAC被取消或配置存在冲突，备份保留。"; }
            finished = true;
        });
    }
    bool ConfirmClose()
    {
        if (!options.profile.gameExecutable.empty() && HasWerBackup(options.profile.gameExecutable))
            return MessageBoxW(window, L"WER配置仍有未恢复备份，后续同名程序崩溃可能继续生成dump。\n建议返回并点击恢复原配置。仍要退出？", L"系统捕获配置尚未恢复", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        return true;
    }
    void Start()
    {
        if (busy) return;
        Options input = options;
        const auto time = timeInput->GetText();
        input.incidentTime = 0;
        if (!time.empty() && !TryParseUtcIso8601(time, input.incidentTime))
        { status->SetText("时间格式不正确，请输入带Z或+08:00的ISO时间。"); return; }
        if (input.incidentTime > CurrentUtcTime() + 5 * TicksPerMinute)
        { status->SetText("发生时间不能在未来。"); return; }
        input.includeEvents = details->IsChecked();
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
            catch (const std::exception&) { std::lock_guard lock(mutex); error = "采集或打包失败，已写出的本地文件会保留。请检查输出目录。"; }
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
                    ? "检查完成，退出码 " + std::to_string(checkOutcome.exitCode) + "。报告：" + checkOutcome.textPath.string()
                    : error);
            }
            else if (completed == Job::Capture || completed == Job::WerRestore)
            {
                captureStatus->SetText(error.empty() ? captureMessage : error);
                status->SetText(error.empty() ? captureMessage : error);
            }
            else status->SetText(!error.empty() ? error : result.cancelled ? "已取消，已生成包含部分信息的ZIP。"
                : result.partial ? "ZIP已生成；部分步骤失败，请查看manifest。" : "ZIP已生成。请检查报告后手动发送。");
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
        if (app->tab == ShellTab::Collection)
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
        if (!app->busy && w < 128 && (!app->kindBox || !app->kindBox->IsOpen()) && (!app->captureMode || !app->captureMode->IsOpen())) app->router.ProcessChar(static_cast<unsigned>(w));
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
            else throw std::invalid_argument("Unsupported GUI option. CLI commands need -batchmode; "
                                             "see ZEnvironmentCollector.exe -batchmode --help");
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
        HWND window = CreateWindowExW(0, klass.lpszClassName, L"环境信息采集 - ZSlate", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 790, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Window creation failed");
        // Window sizing at the monitor's scale (960x740 logical) is
        // Win32::ResizeWindowForDpi, shared with the other ZSlate tools.
        const UINT dpi = static_cast<UINT>(Win32::QueryDpiScaleForWindow(window) * 96.0f);
        app.ApplyDpi(dpi);
        Win32::ResizeWindowForDpi(window, 960, 740, dpi);
        auto backend = std::make_unique<ZSlateD3D11Renderer>(window);
        if (!backend->Init())
        {
            zengine::envcheck::LogError("D3D11 renderer init failed; falling back to -batchmode");
            MessageBoxW(window, L"图形界面启动失败。可运行同目录 ZEnvironmentCollector.exe -batchmode --collect 生成报告。", L"图形初始化失败", MB_OK | MB_ICONWARNING);
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
        MessageBoxW(nullptr, L"无法启动采集界面。请检查项目配置、系统字体及工具分发是否完整。可使用 -batchmode --collect 模式。", L"环境采集", MB_OK | MB_ICONERROR);
        return 30;
    }
}
