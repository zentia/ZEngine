// ============================================================================
// LuaMemPreview — ZEditor 的一个工具套件
// ============================================================================
//
// 原来是一个独立 exe（engine/tools/LuaMemPreview），现在是 ZEditor 里的一套
// 工具：启动器选它、或命令行 `ZEditor -batchmode -suite=LuaMemPreview`。
// 独立 exe 那一份（Win32 宿主 / app icon / 单独的 CMake target）已经删掉 ——
// ZSlate 的 FSlateStandaloneApp 本来就把宿主做完了，再包一层 exe 只是多一份
// 要单独构建、单独分发的东西。
//
// 三种用法（都是套件入口，不再有独立的 argc/argv）：
//   ZEditor -batchmode -suite=LuaMemPreview <in.zasset|目录> [输出] [--recursive]
//                                       导出 _csharp/_userdata/_lua.csv
//   ZEditor -batchmode -suite=LuaMemPreview --stats <file>      按类型统计
//   ZEditor -batchmode -suite=LuaMemPreview --dump-ui <file>    无头 UI 断言
//   ZEditor -suite=LuaMemPreview [file]                         打开 GUI
//
// 消费的是 LuaMemCsvExport 写的 "Count,Size,Type,Stack" CSV，展示：
//   * 按类型的占比条 + 可点的 chip（~100 MB / ~180K 行到底去哪了，只有这个看得出来），
//   * 可排序的虚拟表格，
//   * 选中行的完整分配堆栈。
//
// 输入既可以是 MemoryProfiler 的 .zasset 快照本身，也可以是从它导出的
// _lua.csv。前者是原始来源（-batchmode 就是从它导出 CSV 的），直接打开就省了
// 一步。两条路都走 FSnapshot，产出同一个模型；在参考快照上逐字节一致。
//
//   Ctrl+O 打开，或把文件拖到窗口上。

#include "Editor/ToolSuite/LuaMemPreview/LuaMemPreviewSuite.h"

#include "LuaMemCsv.h"
#include "LuaMemLog.h"
#include "LuaMemTypeDoc.h"
#include "LuaMemWidgets.h"
#include "LuaMemZasset.h"

#include <ZAppShell/AppMain.h>

#include "ZSlate/App/SlateStandaloneApp.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Testing/SlateRecordingRenderer.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef DrawText  // windows.h #defines DrawText -> DrawTextA/W, clashing with ISlateRenderer
#include <commdlg.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>

using namespace LuaMemPreview;
using ZSlate::FSlateDpiScaleRenderer;
using ZSlate::FSlateRecordedText;
using ZSlate::FSlateRecordingRenderer;

// ============================================================================
// Console
// ============================================================================
// The exe is linked /SUBSYSTEM:WINDOWS so double-clicking it in Explorer shows
// only the Slate window, with no console flashing behind it. The trade-off is
// that the headless modes (-batchmode / --stats / --dump-ui) then start with a
// dead stdout, since only the GUI path ever needs a window.
//
// Attaching (not allocating) is what makes them usable from a terminal: the
// output lands in the shell you launched from, instead of a second window that
// appears and vanishes. AllocConsole is the fallback for "launched with no
// console at all" (Explorer, a shortcut) so the output is not simply lost.
//
// Skipped when stdout is already redirected: `> out.txt` / `| more` means the
// caller owns the output, and AttachConsole would silently redirect it away.
namespace
{
// Attaching a console for the headless modes is ZSlate::Win32::EnsureConsole().

// Optional, explicitly requested: without a console of our own the process
// exits as soon as it is done, taking its output with it.
void MaybePause()
{
    std::printf("\nPress Enter to close...");
    std::fflush(stdout);
    std::getchar();
}

// Every headless mode has several exit points, and each one has to offer the
// pause and drain bqlog before it returns - otherwise the output scrolls away
// on the most interesting paths (the failures) and the async log worker loses
// its tail.
int ExitCode(int code, bool pause)
{
    LogFlush();
    if (pause)
        MaybePause();
    return code;
}
}  // namespace

// ============================================================================
// Application
// ============================================================================
// The Win32 host, message pump, DPI handling and font loading are
// ZSlate::FSlateStandaloneApp (see ZSlate/App/SlateStandaloneApp.h); this class
// is only what is specific to viewing a Lua memory snapshot.
class FPreviewApp
{
public:
    ZSlate::FSlateStandaloneApp Host;

    FSnapshot Snapshot;
    FRowView RowView;

    std::shared_ptr<ZSlate::STextBlock> StatusText;
    std::shared_ptr<ZSlate::SEditableTextBox> SearchBox;
    std::shared_ptr<SLuaTypeBar> TypeBar;
    std::shared_ptr<SLuaMemTable> Table;
    std::shared_ptr<SLuaStackDetail> Detail;

    void BuildUI()
    {
        auto root = std::make_shared<ZSlate::SVerticalBox>();

        // ---- Toolbar ------------------------------------------------------
        auto toolbar = std::make_shared<ZSlate::SHorizontalBox>();

        StatusText = std::make_shared<ZSlate::STextBlock>();
        StatusText->Text = "No file loaded  -  Ctrl+O to open, or drop a CSV here";
        StatusText->FontSize = 13.0f;
        StatusText->Color = ZSlate::UIColor(0.72f, 0.72f, 0.76f, 1.0f);
        toolbar->AddSlot(StatusText).Fill(1.0f).SetVAlign(ZSlate::EVerticalAlignment::Center);

        SearchBox = std::make_shared<ZSlate::SEditableTextBox>();
        SearchBox->HintText = "filter stack...";
        SearchBox->FontSize = 12.0f;
        SearchBox->MinWidth = 260.0f;
        SearchBox->OnTextChanged = [this](const std::string& text)
        {
            if (!Snapshot.IsLoaded())
                return;
            RowView.SetSearch(Snapshot, text);
            Table->ResetView();
            RefreshStatus();
        };
        toolbar->AddSlot(SearchBox).AutoSize().SetVAlign(ZSlate::EVerticalAlignment::Center);

        root->AddSlot(toolbar).AutoSize().SetPadding(ZSlate::FMargin(10.0f, 7.0f));

        // ---- Type proportion bar ------------------------------------------
        TypeBar = std::make_shared<SLuaTypeBar>();
        TypeBar->Snapshot = &Snapshot;
        TypeBar->View = &RowView;
        TypeBar->OnTypeClicked = [this](uint16_t type_id)
        {
            // Clicking the active chip clears the filter, so the bar doubles as
            // a toggle instead of needing a separate "clear" affordance.
            const uint16_t current = RowView.GetTypeFilter();
            const uint16_t next = (type_id != FRowView::kAllTypes && type_id == current)
                                      ? FRowView::kAllTypes
                                      : type_id;
            RowView.SetTypeFilter(Snapshot, next);
            Table->ResetView();
            RefreshStatus();
        };
        root->AddSlot(TypeBar).AutoSize();

        // ---- Table --------------------------------------------------------
        Table = std::make_shared<SLuaMemTable>();
        Table->Snapshot = &Snapshot;
        Table->View = &RowView;
        Table->OnSelectionChanged = [this](int64_t row) { Detail->SetRow(row); };
        Table->OnSortChanged = [this] { RefreshStatus(); };
        root->AddSlot(Table).Fill(1.0f);

        // ---- Detail pane --------------------------------------------------
        Detail = std::make_shared<SLuaStackDetail>();
        Detail->Snapshot = &Snapshot;
        Detail->SetRow(-1);
        root->AddSlot(Detail).AutoSize();

        Host.SetRootContent(root);
        Host.GetInputRouter().SetKeyboardFocusWidget(Table.get());
    }

    void RefreshStatus()
    {
        if (!Snapshot.IsLoaded())
            return;

        char buf[512];
        const uint16_t filter = RowView.GetTypeFilter();
        const bool filtered = (filter != FRowView::kAllTypes) || !RowView.GetSearch().empty();

        if (filtered)
        {
            std::snprintf(buf, sizeof(buf),
                          "%s   |   %s rows shown of %s   |   %s of %s   (%s%s)",
                          Snapshot.GetFileName().c_str(),
                          FormatCount(RowView.Size()).c_str(),
                          FormatCount(Snapshot.GetRowCount()).c_str(),
                          FormatBytes(RowView.GetVisibleSize()).c_str(),
                          FormatBytes(Snapshot.GetTotalSize()).c_str(),
                          (filter != FRowView::kAllTypes) ? Snapshot.GetTypeName(filter).c_str() : "search",
                          (filter != FRowView::kAllTypes && !RowView.GetSearch().empty()) ? " + search" : "");
        }
        else
        {
            std::snprintf(buf, sizeof(buf),
                          "%s   |   %s rows   |   %s total   |   %s objects%s",
                          Snapshot.GetFileName().c_str(),
                          FormatCount(Snapshot.GetRowCount()).c_str(),
                          FormatBytes(Snapshot.GetTotalSize()).c_str(),
                          FormatCount(Snapshot.GetTotalCount()).c_str(),
                          Snapshot.IsSelfConsistent() ? "" : "   |   [!] row sum differs from the file's TOTAL row");
        }
        StatusText->Text = buf;
    }

    void LoadFile(const std::string& path)
    {
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!Snapshot.LoadFromFile(path, err))
        {
            LogError("load failed: " + err);
            StatusText->Text = "Load failed: " + err;
            StatusText->Color = ZSlate::UIColor(1.0f, 0.45f, 0.45f, 1.0f);
            RowView.Rebuild(Snapshot);
            Table->ResetView();
            Detail->SetRow(-1);
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        StatusText->Color = ZSlate::UIColor(0.72f, 0.72f, 0.76f, 1.0f);
        SearchBox->SetText("");
        RowView.SetSearch(Snapshot, "");
        RowView.SetTypeFilter(Snapshot, FRowView::kAllTypes);
        RowView.SetSort(Snapshot, EColumn::Size, true);
        Table->ResetView();
        Detail->SetRow(-1);
        RefreshStatus();

        LogInfo("loaded " + path + ": " + std::to_string(Snapshot.GetRowCount()) + " rows, " +
                FormatBytes(Snapshot.GetTotalSize()) + ", parsed in " +
                std::to_string(static_cast<int>(ms)) + " ms");

        Host.SetWindowTitle("Lua Memory Preview - " + Snapshot.GetFileName());
    }

    void OpenFileDialog()
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = Host.GetHwnd();
        ofn.lpstrFilter = "Lua memory snapshot\0*.zasset;*_lua.csv\0"
                          "MemoryProfiler asset\0*.zasset\0"
                          "Lua profile CSV\0*_lua.csv\0"
                          "CSV files\0*.csv\0"
                          "All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = "Open Lua memory snapshot (.zasset or _lua.csv)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn))
            LoadFile(file);
    }

};

namespace
{
// usage 文本里的程序名：现在是 ZEditor 的一个套件，不再是独立 exe。
constexpr const char* kExeLabel = "ZEditor -suite=LuaMemPreview";

void PrintUsage(const char* exeName)
{
    std::printf("LuaMemPreview - viewer and CSV exporter for Lua memory snapshots\n"
                "\n"
                "Usage:\n"
                "  %s [file]                            open <file> (.zasset or _lua.csv) in the GUI\n"
                "  %s -batchmode <in> [out]             export _csharp/_userdata/_lua.csv;\n"
                "                                       <in> is one .zasset or a folder of them\n"
                "                                       (--recursive to walk subdirectories)\n"
                "  %s --stats <file>                    headless per-type breakdown\n"
                "  %s --dump-ui <file>                  headless assertions on the painted UI\n"
                "\n"
                "Options:\n"
                "  --recursive  -batchmode folder: also walk subdirectories\n"
                "  --pause      wait for Enter before exiting (headless modes only)\n"
                "\n"
                "In the GUI: Ctrl+O to open, or drag a file onto the window.\n"
                "\n"
                "-batchmode / --stats / --dump-ui print to the console they were started\n"
                "from. This is a /SUBSYSTEM:WINDOWS executable, but cmd.exe still waits for\n"
                "it, so `%s -batchmode ... && next` in a batch file does what you expect.\n"
                "--pause is the other way to keep the output on screen.\n",
                exeName, exeName, exeName, exeName, exeName);
}
}  // namespace

// WinMain, matching the other ZEngine GUI executables (ZEditor, ZInsights,
// TexPreview): the target is linked /SUBSYSTEM:WINDOWS, so this is the entry
// point the CRT expects and no explicit /ENTRY override is needed.
//
// Arguments come from the wide command line rather than the CRT globals:
// __argv is in the ANSI codepage, so a non-ASCII path arrives already mangled,
// and a path is exactly what -batchmode takes. Same source every other ZEngine
// executable uses (ZAppShell).
// 归一化 + 分发。
//
// `args` 是 argv 形状：args[0] 是程序名（只出现在 usage 文本里），其余是套件
// 收到的参数。修饰开关（--pause / --recursive）可以在任何位置，所以先把它们
// 摘掉再看第一个位置参数是不是子命令 —— 老 CLI 用 "argc 减去修饰开关个数"
// 来定位，`LuaMemPreview -batchmode --recursive <dir>` 会把 "--recursive"
// 当成输入路径。
//
// `batch_context` = 这次是 `-batchmode` 进来的：没写子命令时剩下的就是
// <输入> [输出]，不逼用户把 -batchmode 再写一遍。
int DispatchArgs(const std::vector<std::string>& args, bool batch_context)
{
    bool pause_at_exit = false;
    bool recursive     = false;
    std::vector<std::string> positional;
    for (size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--pause")
        {
            pause_at_exit = true;
            continue;
        }
        if (args[i] == "--recursive")
        {
            recursive = true;
            continue;
        }
        positional.push_back(args[i]);
    }

    const std::string mode = positional.empty() ? std::string() : positional[0];
    const bool explicit_batch =
        (mode == "-batchmode" || mode == "--batchmode" || mode == "--batch");
    const bool is_mode = explicit_batch || mode == "--stats" || mode == "--dump-ui" ||
                         mode == "--help" || mode == "-h" || mode == "-?";
    const std::vector<std::string> params(positional.begin() + (is_mode ? 1 : 0), positional.end());
    const bool csv_export = explicit_batch || (batch_context && !is_mode);

    if (mode == "--help" || mode == "-h" || mode == "-?")
    {
        zengine::appshell::EnsureConsole();
        PrintUsage(kExeLabel);
        return ExitCode(0, pause_at_exit);
    }

    // ---- Batch mode: .zasset(s) -> CSV ------------------------------------
    // Unity-style -batchmode: no window, no D3D11, just the engine Core
    // systems needed to deserialize the MemoryProfiler asset(s). Output is the
    // same three CSVs the former ZProfilerCsvExporter wrote; `_lua.csv` can be
    // opened back in this viewer.
    //
    // The input may be a single snapshot or a folder of them. The folder case
    // is the one worth having: a capture session drops a whole directory of
    // snapshots, and converting them one by one by hand is the whole reason
    // this tool needed a batch mode at all.
    if (csv_export)
    {
        zengine::appshell::EnsureConsole();
        if (params.empty() || params.size() > 2)
        {
            std::printf("Usage: %s -batchmode <snapshot.zasset | folder> [output]\n"
                        "\n"
                        "Output per snapshot:\n"
                        "  <base>_csharp.csv\n"
                        "  <base>_userdata.csv\n"
                        "  <base>_lua.csv\n"
                        "\n"
                        "A snapshot:  [output] is <base>, defaulting to the input path without\n"
                        "             its extension.\n"
                        "A folder:    [output] is a directory, defaulting to the folder itself.\n"
                        "             Every *.zasset in it becomes <dir>/<stem>_lua.csv; add\n"
                        "             --recursive to walk subdirectories.\n",
                        kExeLabel);
            LogError("batchmode: expected <snapshot.zasset | folder> [output]");
            return ExitCode(1, pause_at_exit);
        }

        const std::string input  = params[0];
        const std::string output = (params.size() >= 2) ? params[1] : "";
        const bool folder_mode = IsDirectory(input);
        const bool snapshot_mode = IsSnapshotPath(input);

        // Decide what the argument was *meant* to be before touching the disk:
        // a missing "C:\...\XYZ" is not a snapshot that failed to open, and
        // "cannot open file" sends you looking for a permissions problem that
        // isn't there.
        if (!folder_mode && !snapshot_mode)
        {
            const std::string message = "not a snapshot (.zasset) and not a folder: " + input;
            std::fprintf(stderr, "ERROR: %s\n", message.c_str());
            LogError("batchmode: " + message);
            return ExitCode(1, pause_at_exit);
        }

        std::vector<std::string> written;
        std::vector<std::string> errors;
        const auto t0 = std::chrono::steady_clock::now();

        bool ok = false;
        if (folder_mode)
        {
            ok = ExportFolderToCsv(input, output, recursive, written, errors);
        }
        else
        {
            std::string err;
            ok = ExportZassetToCsv(input, output, written, err);
            if (!ok)
                errors.push_back(err);
        }

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        for (const std::string& e : errors)
            std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        if (!errors.empty())
            std::printf("\n");

        std::printf("Exported CSV files (%.0f ms):\n", ms);
        for (const std::string& p : written)
            std::printf("  %s\n", p.c_str());

        // How many *snapshots* succeeded, not how many files: three CSVs per
        // snapshot is an implementation detail.
        const size_t snapshots = written.size() / 3;
        const std::string summary = folder_mode
            ? ("batch: " + std::to_string(snapshots) + " snapshot(s) from " + input +
               " -> " + (output.empty() ? input : output) +
               (recursive ? " (recursive)" : "") +
               ", " + std::to_string(errors.size()) + " error(s)")
            : ("Exported " + std::to_string(written.size()) + " CSV files from " + input);

        if (ok && errors.empty())
        {
            LogInfo(summary + " in " + std::to_string(static_cast<int>(ms)) + " ms");
            return ExitCode(0, pause_at_exit);
        }

        LogError(summary);
        for (const std::string& e : errors)
            LogError("batchmode: " + e);
        return ExitCode(1, pause_at_exit);
    }

    // ---- Headless UI verification -----------------------------------------
    // Paints one real frame into a recording renderer and asserts on what the
    // widgets emitted. Needs no GPU, no window and no font file, which is also
    // the only way to check virtualisation: a DXGI flip-model swapchain returns
    // a blank surface to PrintWindow, so a screenshot proves nothing.
    if (mode == "--dump-ui" && !params.empty())
    {
        zengine::appshell::EnsureConsole();
        FPreviewApp app;
        ZSlate::SetPlatform(&app.Host);
        app.BuildUI();
        app.LoadFile(params[0]);
        if (!app.Snapshot.IsLoaded())
        {
            std::printf("ERROR: snapshot failed to load\n");
            LogError("dump-ui: snapshot failed to load: " + params[0]);
            return ExitCode(1, pause_at_exit);
        }

        FSlateRecordingRenderer rec;
        app.Host.SetRendererOverride(&rec);
        // Headless: no window, so declare the logical viewport directly. The
        // physical size is the same at scale 1.0.
        app.Host.ApplyDpi(1.0f, 1440, 900);
        const ZSlate::UIRect full(0.0f, 0.0f, 1440.0f, 900.0f);
        rec.Reset();
        ZSlate::SlateApplication::Get().PaintInto(&rec, full);

        const size_t rows = app.Snapshot.GetRowCount();
        const std::string top_type = app.Snapshot.GetTypeName(app.Snapshot.GetTypeBuckets()[0].TypeId);
        const std::string top_size = FormatBytes(app.Snapshot.GetTypeBuckets()[0].Size);
        const std::string total = FormatBytes(app.Snapshot.GetTotalSize());

        std::printf("draw calls  : %zu texts, %zu quads, %d rects\n",
                    rec.Texts.size(), rec.Quads.size(), rec.RectCount);
        std::printf("clip stack  : %d push / %d pop, max depth %d\n",
                    rec.PushClipCount, rec.PopClipCount, rec.MaxClipDepth);

        struct FCheck { const char* Name; bool Ok; };

        // Layout: with Fill honoured, the table must span the full window width,
        // so the STACK column has to start well right of the fixed columns and
        // the rows must reach close to the bottom of the window.
        float widest_text_right = 0.0f;
        float lowest_row_bottom = 0.0f;
        int row_like = 0;  // texts drawn at the table's row height
        for (const FSlateRecordedText& t : rec.Texts)
        {
            widest_text_right = (t.Rect.x + t.Rect.w > widest_text_right) ? t.Rect.x + t.Rect.w : widest_text_right;
            if (t.Rect.y > lowest_row_bottom)
                lowest_row_bottom = t.Rect.y;
            if (t.Text.find("MB") != std::string::npos || t.Text.find("KB") != std::string::npos)
                ++row_like;
        }

        // A correct frame draws only the rows that fit on screen. At 1440x900
        // that is roughly 30 rows x 4 columns plus chrome; a count anywhere
        // near rows*4 (727K) would mean the virtualisation is not working.
        const bool virtualised = rec.Texts.size() < 400;
        const FCheck checks[] = {
            {"clip stack balanced",          rec.IsClipBalanced()},
            {"no clip underflow",            !rec.ClipUnderflow},
            {"virtualised (texts << rows)",  virtualised},
            {"table stretched to width",     widest_text_right > 1000.0f},
            {"rows fill window height",      lowest_row_bottom > 600.0f},
            {"all 11 buckets have a chip",   rec.HasText("vm_other") && rec.HasText("thread")},
            {"status bar shows file name",   rec.HasText(app.Snapshot.GetFileName())},
            {"status bar shows total size",  rec.HasText(total)},
            {"no consistency warning",       !rec.HasText("[!]")},
            {"ALL chip present",             rec.HasText("ALL")},
            {"biggest type chip present",    rec.HasText(top_type) && rec.HasText(top_size)},
            {"header COUNT",                 rec.HasText("COUNT")},
            {"header SIZE sorted desc",      rec.HasText("SIZE  v")},
            {"header TYPE",                  rec.HasText("TYPE")},
            {"header STACK",                 rec.HasText("STACK")},
            {"detail pane placeholder",      rec.HasText("Select a row")},
        };

        // ---- Category documentation ----------------------------------------
        // Hovering a chip must explain what that bucket counts. Checked
        // headlessly because the card is painted *after* the widget tree (it
        // has to escape the type bar's AutoSize slot), so it sits outside
        // PaintInto and a regression there would fail nothing else.
        //
        // Chip hit rects are produced by OnPaint, so the paint above is what
        // makes hover reachable at all.
        const auto find_chip_text = [&rec](const std::string& type_name) -> const ZSlate::FSlateRecordedText*
        {
            // The chip label is "<type>  <size>  <pct>"; the table's TYPE cell
            // is the bare name. Matching the two-space suffix picks the chip.
            const std::string prefix = type_name + "  ";
            for (const ZSlate::FSlateRecordedText& t : rec.Texts)
            {
                if (t.Text.compare(0, prefix.size(), prefix) == 0)
                    return &t;
            }
            return nullptr;
        };

        // Repaints with `type_name`'s chip hovered. card_start is the index of
        // the first text belonging to the card, so the card can be inspected
        // without confusing it with the rest of the UI.
        const auto paint_hovered = [&](const std::string& type_name,
                                       ZSlate::FSlateRecordingRenderer& out,
                                       size_t& card_start) -> bool
        {
            const ZSlate::FSlateRecordedText* chip = find_chip_text(type_name);
            if (chip == nullptr)
                return false;
            // Route through SlateInputRouter, exactly as the frame loop does.
            // Calling TypeBar->OnMouseMove directly would bypass the router and
            // hide a regression in move delivery -- which is precisely the bug
            // this check exists for.
            app.Host.MousePosition() = ZSlate::Vector2(chip->Rect.x + 2.0f,
                                                       chip->Rect.y + chip->Rect.h * 0.5f);
            app.Host.GetInputRouter().ProcessMouse(ZSlate::SlateApplication::Get().GetRootContent(),
                                                   app.Host.MousePosition(), true, false, 0.0f, false);
            out.Reset();
            app.Host.SetRendererOverride(&out);
            ZSlate::SlateApplication::Get().PaintInto(&out, full);
            card_start = out.Texts.size();
            PaintTypeDocCard(&out, full, *app.TypeBar, app.Snapshot);
            return true;
        };

        ZSlate::FSlateRecordingRenderer doc_rec;
        size_t doc_start = 0;
        const bool doc_hovered = paint_hovered(top_type, doc_rec, doc_start);
        const bool doc_shown = doc_hovered && doc_rec.Texts.size() > doc_start;
        const bool doc_titled = doc_shown &&
                                doc_rec.Texts[doc_start].Text.compare(0, top_type.size(), top_type) == 0;

        // The body must be the real doc text, not a placeholder: compare
        // against the first line of what LuaMemTypeDoc actually returns.
        std::string first_line = LuaMemPreview::TypeDetail(top_type);
        const size_t nl = first_line.find('\n');
        if (nl != std::string::npos)
            first_line.resize(nl);
        bool doc_body_ok = doc_shown && !first_line.empty();
        if (doc_body_ok)
        {
            doc_body_ok = false;
            for (size_t i = doc_start; i < doc_rec.Texts.size(); ++i)
            {
                if (doc_rec.Texts[i].Text == first_line)
                {
                    doc_body_ok = true;
                    break;
                }
            }
        }

        // A card that runs off the edge is useless; placement must clamp.
        bool doc_on_screen = doc_shown;
        for (size_t i = doc_start; i < doc_rec.Texts.size(); ++i)
        {
            const ZSlate::UIRect& rc = doc_rec.Texts[i].Rect;
            if (rc.x < full.x - 0.5f || rc.y < full.y - 0.5f ||
                rc.x + rc.w > full.x + full.w + 0.5f || rc.y + rc.h > full.y + full.h + 0.5f)
            {
                doc_on_screen = false;
                break;
            }
        }

        // Every bucket ltracker.h can emit needs an entry, otherwise a newly
        // added bucket would silently show the "unknown" fallback.
        const std::string unknown_summary = LuaMemPreview::TypeSummary("\x01 no such bucket");
        bool all_documented = true;
        for (const FTypeBucket& b : app.Snapshot.GetTypeBuckets())
        {
            const std::string& n = app.Snapshot.GetTypeName(b.TypeId);
            if (LuaMemPreview::TypeSummary(n) == unknown_summary ||
                std::string(LuaMemPreview::TypeDetail(n)).empty())
            {
                all_documented = false;
                break;
            }
        }

        // The two non-GC buckets are the most misread of the set, so the card
        // has to say so rather than leaving them looking like object counts.
        ZSlate::FSlateRecordingRenderer nongc_rec;
        size_t nongc_start = 0;
        const bool nongc_hovered = paint_hovered("string_table", nongc_rec, nongc_start);
        bool nongc_marked = false;
        if (nongc_hovered && nongc_rec.Texts.size() > nongc_start)
            nongc_marked = nongc_rec.Texts[nongc_start].Text.find("GC") != std::string::npos;

        // Hover off must take the card away; a stuck card hides the table.
        // Done last so the hover state is clean for the DPI pass below.
        // WM_MOUSELEAVE: cursor outside the window, so the hover path is empty
        // and the router sends OnMouseLeave to the bar.
        app.Host.MousePosition() = ZSlate::Vector2(-1.0f, -1.0f);
        app.Host.GetInputRouter().ProcessMouse(ZSlate::SlateApplication::Get().GetRootContent(),
                                               app.Host.MousePosition(), false, false, 0.0f, false);
        ZSlate::FSlateRecordingRenderer bare_rec;
        app.Host.SetRendererOverride(&bare_rec);
        bare_rec.Reset();
        ZSlate::SlateApplication::Get().PaintInto(&bare_rec, full);
        const size_t bare_count = bare_rec.Texts.size();
        PaintTypeDocCard(&bare_rec, full, *app.TypeBar, app.Snapshot);
        const bool doc_hidden = bare_rec.Texts.size() == bare_count;

        const FCheck doc_checks[] = {
            {"doc: chip hover found",        doc_hovered},
            {"doc: card shown on hover",     doc_shown},
            {"doc: card titled by type",     doc_titled},
            {"doc: card body is real text",  doc_body_ok},
            {"doc: card clamped on screen",  doc_on_screen},
            {"doc: every bucket documented", all_documented},
            {"doc: non-GC bucket flagged",   nongc_marked},
            {"doc: no card without hover",   doc_hidden},
        };

        // ---- DPI scaling ---------------------------------------------------
        // Repaint the identical UI through FSlateDpiScaleRenderer at 150% and compare
        // against the 100% pass above. A correct decorator is a pure similarity
        // transform: same draw calls in the same order, every rect and every
        // font size exactly 1.5x. Font size matters most -- it is what makes the
        // atlas rasterize bigger glyphs instead of stretching a 14px bitmap.
        constexpr float kTestScale = 1.5f;
        FSlateRecordingRenderer scaled_rec;
        FSlateDpiScaleRenderer dpi(&scaled_rec, kTestScale);
        // Logical window stays 1440x900 so layout must come out identical; the
        // physical surface would be 2160x1350.
        // Same logical 1440x900 viewport as the 100% pass; the decorator turns
        // it into a 2160x1350 physical surface.
        app.Host.ApplyDpi(kTestScale, static_cast<int>(1440.0f * kTestScale),
                          static_cast<int>(900.0f * kTestScale));
        app.Host.SetRendererOverride(&dpi);
        scaled_rec.Reset();
        ZSlate::SlateApplication::Get().PaintInto(&dpi, full);

        bool dpi_same_count = scaled_rec.Texts.size() == rec.Texts.size();
        bool dpi_font_scaled = dpi_same_count && !rec.Texts.empty();
        bool dpi_rect_scaled = dpi_same_count && !rec.Texts.empty();
        float worst_font_err = 0.0f;
        float worst_rect_err = 0.0f;
        if (dpi_same_count)
        {
            for (size_t i = 0; i < rec.Texts.size(); ++i)
            {
                const FSlateRecordedText& a = rec.Texts[i];
                const FSlateRecordedText& b = scaled_rec.Texts[i];
                if (a.Text != b.Text)
                {
                    dpi_same_count = false;  // order/content drift
                    break;
                }
                const float fe = std::fabs(b.FontSize - a.FontSize * kTestScale);
                const float re = std::fabs(b.Rect.x - a.Rect.x * kTestScale) +
                                 std::fabs(b.Rect.y - a.Rect.y * kTestScale) +
                                 std::fabs(b.Rect.w - a.Rect.w * kTestScale) +
                                 std::fabs(b.Rect.h - a.Rect.h * kTestScale);
                worst_font_err = (fe > worst_font_err) ? fe : worst_font_err;
                worst_rect_err = (re > worst_rect_err) ? re : worst_rect_err;
            }
            dpi_font_scaled = worst_font_err < 0.01f;
            dpi_rect_scaled = worst_rect_err < 0.05f;
        }
        // MeasureText must round-trip back to logical units, otherwise desired
        // sizes inflate by Scale and the layout drifts at high DPI.
        const ZSlate::Vector2 logical_m = scaled_rec.MeasureText("MMMMMMMM", 14.0f);
        const ZSlate::Vector2 via_dpi = dpi.MeasureText("MMMMMMMM", 14.0f);
        const bool measure_roundtrip = std::fabs(via_dpi.x - logical_m.x) < 0.01f &&
                                       std::fabs(via_dpi.y - logical_m.y) < 0.01f;

        const FCheck dpi_checks[] = {
            {"dpi: same draw calls at 150%", dpi_same_count},
            {"dpi: font sizes exactly 1.5x", dpi_font_scaled},
            {"dpi: rects exactly 1.5x",      dpi_rect_scaled},
            {"dpi: MeasureText round-trips", measure_roundtrip},
        };

        int failed = 0;
        std::printf("\n");
        for (const FCheck& c : checks)
        {
            if (!c.Ok)
                ++failed;
            std::printf("  %s  %s\n", c.Ok ? "PASS" : "FAIL", c.Name);
        }
        for (const FCheck& c : doc_checks)
        {
            if (!c.Ok)
                ++failed;
            std::printf("  %s  %s\n", c.Ok ? "PASS" : "FAIL", c.Name);
        }
        for (const FCheck& c : dpi_checks)
        {
            if (!c.Ok)
                ++failed;
            std::printf("  %s  %s\n", c.Ok ? "PASS" : "FAIL", c.Name);
        }
        std::printf("\ndpi 150%%: %zu texts (vs %zu at 100%%), worst font err %.4f, worst rect err %.4f\n",
                    scaled_rec.Texts.size(), rec.Texts.size(), worst_font_err, worst_rect_err);
        std::printf("\nrows in snapshot: %zu   texts emitted: %zu   ratio 1:%.0f\n",
                    rows, rec.Texts.size(),
                    rec.Texts.empty() ? 0.0 : static_cast<double>(rows) / static_cast<double>(rec.Texts.size()));

        // Show the first few row texts so the column content can be eyeballed.
        std::printf("\nfirst recorded texts (layout order):\n");
        for (size_t i = 0; i < rec.Texts.size() && i < 22; ++i)
        {
            const FSlateRecordedText& t = rec.Texts[i];
            std::string s = t.Text;
            if (s.size() > 60)
                s = s.substr(0, 57) + "...";
            std::printf("  [%6.0f,%6.0f] %s\n", t.Rect.x, t.Rect.y, s.c_str());
        }
        if (pause_at_exit)
            MaybePause();
        return ExitCode((failed == 0) ? 0 : 4, pause_at_exit);
    }

    // ---- Headless stats ---------------------------------------------------
    // Keeps the data layer verifiable without a window, which is also what a
    // script or CI job wants when it just needs the per-type split.
    if (mode == "--stats" && !params.empty())
    {
        zengine::appshell::EnsureConsole();
        FSnapshot snap;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!snap.LoadFromFile(params[0], err))
        {
            std::printf("ERROR: %s\n", err.c_str());
            LogError("stats: " + err);
            return ExitCode(1, pause_at_exit);
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        std::printf("file        : %s\n", snap.GetFileName().c_str());
        std::printf("parse time  : %.0f ms\n", ms);
        std::printf("data rows   : %zu\n", snap.GetRowCount());
        std::printf("objects     : %llu\n", static_cast<unsigned long long>(snap.GetTotalCount()));
        std::printf("total size  : %s (%llu bytes)\n", FormatBytes(snap.GetTotalSize()).c_str(),
                    static_cast<unsigned long long>(snap.GetTotalSize()));

        if (snap.HasDeclaredTotal())
        {
            std::printf("declared    : %s (%llu bytes)  [%s]\n",
                        FormatBytes(snap.GetDeclaredTotal()).c_str(),
                        static_cast<unsigned long long>(snap.GetDeclaredTotal()),
                        snap.GetDeclaredTotalOrigin());
            const long long diff = static_cast<long long>(snap.GetTotalSize()) -
                                   static_cast<long long>(snap.GetDeclaredTotal());
            std::printf("consistency : %s (row sum - declared = %lld bytes)\n",
                        snap.IsSelfConsistent() ? "OK" : "MISMATCH", diff);
        }
        else
        {
            std::printf("declared    : (none - exported without a summary row)\n");
        }

        std::printf("\n%-14s %12s %10s %10s %9s\n", "TYPE", "SIZE", "PCT", "OBJECTS", "ROWS");
        const uint64_t total = snap.GetTotalSize() ? snap.GetTotalSize() : 1;
        for (const FTypeBucket& b : snap.GetTypeBuckets())
        {
            std::printf("%-14s %12s %9.2f%% %10s %9u\n",
                        snap.GetTypeName(b.TypeId).c_str(),
                        FormatBytes(b.Size).c_str(),
                        static_cast<double>(b.Size) * 100.0 / static_cast<double>(total),
                        FormatCount(b.Count).c_str(),
                        b.Rows);
        }

        // Sanity: the buckets must re-add to the grand total.
        uint64_t bucket_sum = 0;
        for (const FTypeBucket& b : snap.GetTypeBuckets())
            bucket_sum += b.Size;
        std::printf("%-14s %12s\n", "(sum)", FormatBytes(bucket_sum).c_str());
        if (bucket_sum != snap.GetTotalSize())
        {
            std::printf("ERROR: bucket sum != total\n");
            LogError("stats: bucket sum != total for " + params[0]);
            return ExitCode(2, pause_at_exit);
        }
        return ExitCode(snap.IsSelfConsistent() ? 0 : 3, pause_at_exit);
    }

    // ---- GUI ---------------------------------------------------------------
    // Window, message pump, DPI awareness / WM_DPICHANGED and the font atlas
    // are all FSlateStandaloneApp now; awareness is declared inside
    // Initialize(), i.e. before the first window exists.
    FPreviewApp app;

    ZSlate::FStandaloneAppDesc desc;
    desc.Title  = "Lua Memory Preview";
    desc.Width  = 1440.0f;
    desc.Height = 900.0f;
    // Monospace: allocation stacks are file:line lists and align far better in
    // a fixed-pitch face.
    desc.Font   = ZSlate::Win32::ESystemFont::Mono;
    // IDI_ICON1 is defined by z_embed_app_icon() in CMakeLists.txt, which also
    // embeds the .ico in the exe. Both halves are needed: the .rc covers
    // Explorer, these cover the title bar / Alt-Tab / the taskbar button.
#ifdef IDI_ICON1
    desc.LargeIconResId   = IDI_ICON1;
    desc.SmallIconResId   = IDI_ICON1;
#endif
    desc.AcceptFileDrops  = true;
    // ClearBeforePaint stays on: the toolbar row has no background widget of
    // its own, so the whole viewport is filled before the tree paints.

    if (!app.Host.Initialize(desc))
        return 1;

    app.BuildUI();

    // Drag a snapshot onto the window, or Ctrl+O for the file dialog.
    app.Host.OnDropFile = [&app](const std::string& path)
    {
        app.LoadFile(path);
        return true;
    };
    app.Host.OnKey = [&app](unsigned vk, bool down) -> bool
    {
        if (!down)
            return false;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && vk == 'O')
        {
            app.OpenFileDialog();
            return true;
        }
        // ZSlate's EKey has no page keys, so drive the table directly.
        if (vk == VK_PRIOR) { app.Table->ScrollByPage(-1); return true; }
        if (vk == VK_NEXT)  { app.Table->ScrollByPage(+1); return true; }
        return false;
    };
    // The category card has to escape the type bar's AutoSize slot to overlap
    // the table below it, so it is painted after the tree instead of from a
    // widget. The host still sends it before EndFrame, which is what makes the
    // draw calls land.
    app.Host.OnPostPaint = [&app](ZSlate::ISlateRenderer* renderer, const ZSlate::UIRect& viewport)
    {
        PaintTypeDocCard(renderer, viewport, *app.TypeBar, app.Snapshot);
    };

    if (!params.empty())
        app.LoadFile(params[0]);

    return app.Host.Run();
}

// ============================================================================
// IToolSuite
// ============================================================================
namespace ZEditor
{

const ToolSuiteDesc& LuaMemPreviewSuite::Desc() const
{
    static const ToolSuiteDesc kDesc {
        "LuaMemPreview",
        "Lua 内存快照",
        "Lua 内存快照（.zasset / _lua.csv）查看与 CSV 导出",
        /* needs_project  = */ false,
        /* supports_batch = */ true,
    };
    return kDesc;
}

int LuaMemPreviewSuite::RunBatch(const std::vector<std::string>& args)
{
    // ZEditor 已经挂过控制台（见 Main.cpp 的 RunBatchMode）；这里再把参数还原
    // 成 argv 形状：args[0] 是程序名，其余是套件自己的参数。
    std::vector<std::string> argv_style{kExeLabel};
    for (const std::string& a : args)
    {
        if (a.rfind("-suite=", 0) == 0) continue;  // 选套件用的，不是套件的参数
        argv_style.push_back(a);
    }
    return DispatchArgs(argv_style, /*batch_context=*/true);
}

int LuaMemPreviewSuite::RunInteractive(const std::filesystem::path& /*project_file*/,
                                       const std::vector<std::string>& args)
{
    std::vector<std::string> argv_style{kExeLabel};
    for (const std::string& a : args)
    {
        if (a.rfind("-suite=", 0) == 0) continue;
        argv_style.push_back(a);
    }
    return DispatchArgs(argv_style, /*batch_context=*/false);
}

namespace
{
    // 自注册。静态对象在 main() 之前构造；注册表容器是函数内静态，
    // 第一次访问时一定已经构造好了（见 ToolSuiteRegistry.cpp）。
    struct LuaMemPreviewRegistrar
    {
        LuaMemPreviewRegistrar() { RegisterToolSuite(std::make_unique<LuaMemPreviewSuite>()); }
    };
    const LuaMemPreviewRegistrar g_lua_mem_preview_registrar;
}   // namespace

}   // namespace ZEditor
