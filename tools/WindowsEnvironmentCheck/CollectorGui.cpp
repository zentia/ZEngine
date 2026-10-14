#include "Collection.h"
#include "CollectorScrollPanel.h"
#include "CrashCapture.h"
#include "WerCapture.h"
#include "ToolVersion.h"
#include <commdlg.h>
#include <shlobj.h>
#include "DiagnosisUtil.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SComboBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
using namespace ZSlate;
using namespace zengine::envcheck::collector;
using namespace zengine::envcheck::diagnosis;
// The widget tree works in DPI-independent logical units (96 DPI = 1.0), so every
// size below is written once at its 100% value. `scaled` converts to physical
// pixels on the way to `renderer` and snaps edges to whole pixels, so neighbouring
// boxes stay seamless at 125%/150%. Win32 coordinates are physical; they are
// divided by `dpiScale` at the WindowProc boundary (see CollectorApp::Logical).
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
class CollectorApp
{
public:
    Platform platform;
    ZSlateFontAtlas fonts;
    SlateInputRouter router;
    Options options;
    HWND window = nullptr;
    std::unique_ptr<ZSlateD3D11Renderer> backend;
    std::shared_ptr<SVerticalBox> column;
    std::shared_ptr<STextBlock> status, timeLabel;
    std::shared_ptr<SComboBox<IncidentKind>> kindBox;
    std::shared_ptr<SComboBox<int>> captureMode;
    std::shared_ptr<SEditableTextBox> timeInput;
    std::shared_ptr<SCheckBox> details;
    std::filesystem::path procDumpPath, targetPath;
    std::shared_ptr<STextBlock> procDumpLabel, targetLabel, captureStatus;
    std::string captureMessage;
    bool capturing = false;
    std::atomic_bool cancel{false}, finished{false}, cancellable{false};
    std::thread worker;
    std::mutex mutex;
    std::string progress, error;
    Result result;
    Result captureReport;
    bool busy = false, closing = false, minimized = false;
    bool suppressPointerUntilRelease = false;
    ~CollectorApp()
    {
        cancel = true;
        if (worker.joinable()) worker.join();
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
    bool InitFonts()
    {
        wchar_t windows[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windows, MAX_PATH)) return false;
        auto dir = std::filesystem::path(windows) / L"Fonts";
        if (!fonts.LoadFromFile(WideToUtf8((dir / L"segoeui.ttf").wstring()))) return false;
        if (!fonts.AddFallbackFont(WideToUtf8((dir / L"msyh.ttc").wstring()))
            && !fonts.AddFallbackFont(WideToUtf8((dir / L"simsun.ttc").wstring()))) return false;
        platform.renderer.SetFontAtlas(&fonts);
        return true;
    }
    void Build()
    {
        column = std::make_shared<SVerticalBox>();
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
        Text("发生时间（可留空自动找记录；输入需带时区）", 16);
        timeInput = std::make_shared<SEditableTextBox>();
        timeInput->FontSize = 18; timeInput->MinWidth = 560;
        timeInput->SetHintText("2026-10-12T18:30:00+08:00");
        timeInput->SetText(FormatUtcIso8601(options.incidentTime));
        AddWidget(timeInput, 6);
        timeLabel = Button("填写当前时间", [this] { if (!busy) timeInput->SetText(FormatUtcIso8601(CurrentUtcTime())); });
        details = std::make_shared<SCheckBox>();
        details->SetLabel("同意附带事件明细");
        details->SetChecked(true);
        details->FontSize = 16; details->BoxSize = 18;
        details->Size = Vector2(260, 28);
        AddWidget(details, 9);
        Button("开始采集并生成 ZIP", [this] { Start(); });
        Button("取消采集（打包阶段不可取消）", [this] { if (busy && cancellable.load()) cancel = true; });
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
        Text("WER仅配置当前exe名称；需要UAC，结束时恢复，不附加游戏。", 16);
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
        Button("安全停止捕获 / 取消后处理", [this] { if (busy && capturing && cancellable.load()) cancel = true; });
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
        auto scroll = std::make_shared<SScrollBox>(); scroll->AddChild(column);
        auto scrollBar = std::make_shared<SScrollBar>();
        scroll->SetScrollBar(scrollBar);
        scrollBar->Thickness = 16;
        scrollBar->MinThumbLength = 32;
        scroll->ScrollSpeed = 40;
        auto root = std::make_shared<CollectorScrollPanel>(scroll, scrollBar);
        SlateApplication::Get().SetRootContent(root);
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
                L"，DumpType=2，保留2份，输出到本次报告的system-dumps私有目录。\n配置按exe名称生效，可能影响其它同名程序；需要UAC授权，结束时再次请求UAC恢复原设置。\n完整内存含敏感数据，占用数GB，不自动上传。配置成功不保证产生dump。\n不附加游戏，不终止WER；异常退出可用恢复按钮处理。\n\n是否同意？"
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
        capturing = true; busy = true; cancel = false; finished = false; cancellable = true;
        captureMode->ClosePopup(); captureMode->Visibility = EVisibility::HitTestInvisible;
        kindBox->ClosePopup(); kindBox->Visibility = EVisibility::HitTestInvisible;
        timeInput->Visibility = EVisibility::HitTestInvisible; details->Visibility = EVisibility::HitTestInvisible;
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
        capturing = true; busy = true; finished = false; cancellable = false;
        captureMode->ClosePopup(); captureMode->Visibility = EVisibility::HitTestInvisible;
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
        capturing = false;
        Options input = options;
        const auto time = timeInput->GetText();
        input.incidentTime = 0;
        if (!time.empty() && !TryParseUtcIso8601(time, input.incidentTime))
        { status->SetText("时间格式不正确，请输入带Z或+08:00的ISO时间。"); return; }
        if (input.incidentTime > CurrentUtcTime() + 5 * TicksPerMinute)
        { status->SetText("发生时间不能在未来。"); return; }
        input.includeEvents = details->IsChecked();
        cancel = false; finished = false; cancellable = true; busy = true;
        captureMode->ClosePopup(); captureMode->Visibility = EVisibility::HitTestInvisible;
        { std::lock_guard lock(mutex); progress = "准备采集"; error.clear(); }
        timeInput->Visibility = EVisibility::HitTestInvisible;
        details->Visibility = EVisibility::HitTestInvisible;
        kindBox->ClosePopup();
        kindBox->Visibility = EVisibility::HitTestInvisible;
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
        { std::lock_guard lock(mutex); status->SetText(progress + " …"); if (capturing) captureStatus->SetText(progress + " …"); }
        if (finished.load())
        {
            worker.join(); busy = false;
            timeInput->Visibility = EVisibility::Visible; details->Visibility = EVisibility::Visible;
            kindBox->Visibility = EVisibility::Visible;
            captureMode->Visibility = EVisibility::Visible;
            std::lock_guard lock(mutex);
            if (capturing)
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
        router.ProcessMouse(SlateApplication::Get().GetRootContent(), platform.mouse,
            platform.mouse.x >= 0 && platform.mouse.y >= 0 && platform.mouse.x < platform.size.x && platform.mouse.y < platform.size.y,
            platform.down, wheel);
    }
};
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto* app = reinterpret_cast<CollectorApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        app = static_cast<CollectorApp*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
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
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        CollectorApp app;
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Command line unavailable");
        std::unique_ptr<wchar_t*, decltype(&LocalFree)> owned(argv, &LocalFree);
        auto defaultProfile = ExecutableDirectory() / L"profiles" / L"default.ini";
        if (std::filesystem::is_regular_file(defaultProfile)) app.options.profile = LoadProfile(defaultProfile);
        for (int i = 1; i < argc; ++i)
        {
            auto value = [&]() -> std::wstring { if (++i >= argc) throw std::invalid_argument("Missing value"); return argv[i]; };
            std::wstring key = argv[i];
            if (key == L"--profile") app.options.profile = LoadProfile(value());
            else if (key == L"--output-dir") app.options.outputDirectory = value();
            else if (key == L"--incident-time") { if (!TryParseUtcIso8601(WideToUtf8(value()), app.options.incidentTime)) throw std::invalid_argument("Invalid incident time"); }
            else throw std::invalid_argument("Unsupported GUI option");
        }
        SetPlatform(&app.platform);
        if (!app.InitFonts()) throw std::runtime_error("Required system fonts unavailable");
        app.Build();
        WNDCLASSW klass{}; klass.lpfnWndProc = WindowProc; klass.hInstance = instance;
        klass.lpszClassName = L"ZEnvironmentCollector"; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&klass)) throw std::runtime_error("Window registration failed");
        HWND window = CreateWindowExW(0, klass.lpszClassName, L"环境信息采集 - ZSlate", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 790, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Window creation failed");
        const UINT dpi = GetDpiForWindow(window);
        app.ApplyDpi(dpi);
        RECT initial{0, 0, MulDiv(960, dpi, 96), MulDiv(740, dpi, 96)};
        AdjustWindowRectExForDpi(&initial, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        SetWindowPos(window, nullptr, 0, 0, initial.right - initial.left, initial.bottom - initial.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        auto backend = std::make_unique<ZSlateD3D11Renderer>(window);
        if (!backend->Init())
        {
            MessageBoxW(window, L"图形界面启动失败。可运行同目录 ZWindowsEnvironmentCheck.exe --collect 生成报告。", L"图形初始化失败", MB_OK | MB_ICONWARNING);
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
                app.kindBox->PaintPopup(&scaled, viewport);
                app.captureMode->PaintPopup(&scaled, viewport);
                scaled.EndFrame();
                app.backend->Render(app.platform.renderer, &app.fonts);
            }
            Sleep(app.minimized ? 50 : 10);
        }
        return 0;
    }
    catch (const std::exception&)
    {
        MessageBoxW(nullptr, L"无法启动采集界面。请检查项目配置、系统字体及工具分发是否完整。可使用CLI的 --collect 模式。", L"环境采集", MB_OK | MB_ICONERROR);
        return 30;
    }
}
