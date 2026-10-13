// ============================================================================
// LuaMemPreview - standalone ZSlate viewer for Lua memory snapshot CSVs
// ============================================================================
//
// Consumes the "Count,Size,Type,Stack" CSV written by ProfilerCsvExport (the
// Lua section of a MemoryProfiler dump) and shows:
//   * a per-type proportion bar with clickable chips (the only practical way to
//     see where ~100 MB across ~180K rows actually went),
//   * a sortable, virtualised table,
//   * the full multi-line allocation stack of the selected row.
//
// Why a standalone executable rather than a ZEditor window: every ZSlate
// *window* under Source/Editor/EditorWindow is currently excluded from the
// build (see Source/Editor/CMakeLists.txt - "Excluded until
// UIRenderer->ISlateRenderer migration ... is complete"). Hosting this inside
// the editor would mean waiting on that migration. A standalone target links
// ZSlate + its D3D11 backend directly, exactly like Tools/TexPreview, and also
// matches how the data is consumed: the CSV is produced on a device or client
// and analysed on a desktop that need not run the editor at all.
//
// Input: either the MemoryProfiler `.zasset` snapshot itself or the `_lua.csv`
// exported from it. The asset is the original - ProfilerCsvExport derives the
// CSV from it - so opening it directly drops the export step. Both paths go
// through FSnapshot and produce an identical model; on the reference snapshot
// they agree to the byte.
//
// Usage:
//   LuaMemPreview.exe [path\to\LuaProfile_xxx.zasset | ..._lua.csv]
//   LuaMemPreview.exe --stats <file>     headless: per-type breakdown
//   LuaMemPreview.exe --dump-ui <file>   headless: assert on the painted tree
//   Ctrl+O to open, or drag a file onto the window.

#include "LuaMemCsv.h"
#include "LuaMemWidgets.h"

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Testing/SlateRecordingRenderer.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef DrawText  // windows.h #defines DrawText -> DrawTextA/W, clashing with ISlateRenderer
#include <commdlg.h>
#include <shellapi.h>

using namespace LuaMemPreview;
using ZSlate::FSlateDpiScaleRenderer;
using ZSlate::FSlateRecordedText;
using ZSlate::FSlateRecordingRenderer;

// ============================================================================
// Platform bridge
// ============================================================================
class FPreviewPlatform : public ZSlate::ISlatePlatform
{
public:
    ZSlate::ZSlateBatchedRenderer Renderer;
    // When set, paint goes here instead (headless --dump-ui verification), so
    // the checks run against the real widget tree rather than a stand-in.
    ZSlate::ISlateRenderer* RendererOverride {nullptr};
    bool MouseDown[3] {};
    // Logical (DPI-independent) units -- see ZSlate/Renderer/SlateDpiScaleRenderer.h.
    // The swapchain and all Win32 message coordinates stay physical; conversion
    // happens at the boundary (WndProc divides, FSlateDpiScaleRenderer multiplies).
    ZSlate::Vector2 MousePos {-1.0f, -1.0f};
    ZSlate::Vector2 WinSize {1440.0f, 900.0f};
    float WheelDelta {0.0f};
    float DpiScale {1.0f};

    ZSlate::ISlateRenderer* GetRenderer() override
    {
        return (RendererOverride != nullptr) ? RendererOverride
                                             : static_cast<ZSlate::ISlateRenderer*>(&Renderer);
    }
    ZSlate::Vector2 GetMousePosition() const override { return MousePos; }
    bool IsMouseButtonDown(int b) const override { return (b >= 0 && b < 3) ? MouseDown[b] : false; }
    bool IsKeyDown(int) const override { return false; }
    ZSlate::Vector2 GetWindowSize() const override { return WinSize; }
    float GetDpiScale() const override { return DpiScale; }
    float GetTimeSeconds() const override
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
};

// ============================================================================
// Application
// ============================================================================
class FPreviewApp
{
public:
    FPreviewPlatform Platform;
    ZSlate::ZSlateFontAtlas FontAtlas;
    ZSlate::SlateInputRouter Input;
    // Sits between SlateApplication and the batched renderer: the widget tree
    // lays out in logical units, this converts to physical pixels and -- the
    // reason text was blurry -- scales font_size *before* the atlas rasterises,
    // so glyphs are baked at the physical size instead of stretched.
    FSlateDpiScaleRenderer ScaledRenderer {&Platform.Renderer, 1.0f};

    FSnapshot Snapshot;
    FRowView RowView;

    std::shared_ptr<ZSlate::STextBlock> StatusText;
    std::shared_ptr<ZSlate::SEditableTextBox> SearchBox;
    std::shared_ptr<SLuaTypeBar> TypeBar;
    std::shared_ptr<SLuaMemTable> Table;
    std::shared_ptr<SLuaStackDetail> Detail;

    HWND Hwnd {nullptr};

    void InitFonts()
    {
        // Monospace first: allocation stacks are file:line lists and align far
        // better in a fixed-pitch face.
        const char* primary[] = {
            "C:\\Windows\\Fonts\\consola.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
        };
        for (const char* f : primary)
        {
            if (FontAtlas.LoadFromFile(f))
                break;
        }
        if (FontAtlas.IsLoaded())
        {
            // Lua source paths can contain CJK characters.
            const char* cjk[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simsun.ttc"};
            for (const char* f : cjk)
            {
                if (FontAtlas.AddFallbackFont(f))
                    break;
            }
        }
        Platform.Renderer.SetFontAtlas(FontAtlas.IsLoaded() ? &FontAtlas : nullptr);
    }

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

        ZSlate::SlateApplication::Get().SetRootContent(root);
        Input.SetKeyboardFocusWidget(Table.get());
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

        std::printf("[LuaMemPreview] %s: %zu rows, %s, parsed in %.0f ms\n",
                    Snapshot.GetFileName().c_str(), Snapshot.GetRowCount(),
                    FormatBytes(Snapshot.GetTotalSize()).c_str(), ms);

        if (Hwnd != nullptr)
        {
            const std::string title = "Lua Memory Preview - " + Snapshot.GetFileName();
            SetWindowTextA(Hwnd, title.c_str());
        }
    }

    void OpenFileDialog()
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = Hwnd;
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

    // Physical client size -> logical layout size. Called on creation, on
    // WM_SIZE and on WM_DPICHANGED (moving the window between monitors of
    // different scale factors re-bakes the glyphs at the new size).
    void ApplyDpi(float scale, int physical_w, int physical_h)
    {
        const float s = (scale > 0.0f) ? scale : 1.0f;
        Platform.DpiScale = s;
        ScaledRenderer.SetScale(s);
        Platform.WinSize = ZSlate::Vector2(static_cast<float>(physical_w) / s,
                                           static_cast<float>(physical_h) / s);
    }

    void RunFrame()
    {
        auto root = ZSlate::SlateApplication::Get().GetRootContent();
        if (root)
        {
            const bool over = Platform.MousePos.x >= 0.0f && Platform.MousePos.y >= 0.0f;
            Input.ProcessMouse(root, Platform.MousePos, over, Platform.MouseDown[0],
                               Platform.WheelDelta, Platform.MouseDown[1]);
            Platform.WheelDelta = 0.0f;  // consumed
        }

        // BeginFrame/EndFrame are forwarded by the decorator; going through it
        // for everything keeps one coordinate space in this function.
        ScaledRenderer.BeginFrame();
        const ZSlate::UIRect full(0.0f, 0.0f, Platform.WinSize.x, Platform.WinSize.y);
        // Base fill: the toolbar row has no background widget of its own.
        ScaledRenderer.DrawQuad(full, ZSlate::UIColor(0.086f, 0.086f, 0.098f, 1.0f));
        ZSlate::SlateApplication::Get().PaintInto(&ScaledRenderer, full);
        ScaledRenderer.EndFrame();
    }
};

// ============================================================================
// Win32 host
// ============================================================================
namespace
{
ZSlate::EKey MapVirtualKey(WPARAM vk)
{
    switch (vk)
    {
    case VK_UP:     return ZSlate::EKey::Up;
    case VK_DOWN:   return ZSlate::EKey::Down;
    case VK_LEFT:   return ZSlate::EKey::Left;
    case VK_RIGHT:  return ZSlate::EKey::Right;
    case VK_HOME:   return ZSlate::EKey::Home;
    case VK_END:    return ZSlate::EKey::End;
    case VK_RETURN: return ZSlate::EKey::Enter;
    case VK_ESCAPE: return ZSlate::EKey::Escape;
    case VK_TAB:    return ZSlate::EKey::Tab;
    case VK_BACK:   return ZSlate::EKey::Backspace;
    case VK_DELETE: return ZSlate::EKey::Delete;
    case VK_SPACE:  return ZSlate::EKey::Space;
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<ZSlate::EKey>(static_cast<int>(ZSlate::EKey::A) + static_cast<int>(vk - 'A'));
    return ZSlate::EKey::Unknown;
}

// ----------------------------------------------------------------------------
// DPI awareness
// ----------------------------------------------------------------------------
// Must run before the first window exists -- a process that has not declared
// awareness gets DPI *virtualisation*: it renders at 96 DPI and the compositor
// bitmap-stretches the whole window to the monitor scale (1.5x here), which is
// a bilinear resample and exactly why the text looked soft. Declaring awareness
// turns that off; FSlateDpiScaleRenderer then does the scaling properly.
//
// Both entry points are resolved dynamically: SetProcessDpiAwarenessContext is
// Win10 1703+ and GetDpiForWindow is 1607+, so a static import would stop the
// exe from loading on anything older.
void DeclareDpiAwareness()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 != nullptr)
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto set_ctx = reinterpret_cast<SetCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (set_ctx != nullptr && set_ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))))
            return;
    }
    // Pre-1703 fallback: system-wide awareness. No per-monitor updates, but it
    // still stops the stretch on the primary monitor.
    SetProcessDPIAware();
}

float QueryDpiScaleForWindow(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 != nullptr)
    {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        auto get_dpi = reinterpret_cast<GetDpiFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (get_dpi != nullptr)
        {
            const UINT dpi = get_dpi(hwnd);
            if (dpi != 0)
                return static_cast<float>(dpi) / 96.0f;
        }
    }
    // Fallback: device caps on the window DC.
    HDC dc = GetDC(hwnd);
    float scale = 1.0f;
    if (dc != nullptr)
    {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0)
            scale = static_cast<float>(dpi) / 96.0f;
        ReleaseDC(hwnd, dc);
    }
    return scale;
}

struct FWinCtx
{
    FPreviewApp* App {nullptr};
    ZSlate::ZSlateD3D11Renderer* Backend {nullptr};
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    FWinCtx* ctx = nullptr;
    if (msg == WM_CREATE)
    {
        ctx = reinterpret_cast<FWinCtx*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    }
    else
    {
        ctx = reinterpret_cast<FWinCtx*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    FPreviewApp* app = (ctx != nullptr) ? ctx->App : nullptr;

    switch (msg)
    {
    case WM_SIZE:
        if (app != nullptr)
        {
            // lp is physical pixels; the swapchain wants those, the widget tree
            // wants logical.
            app->ApplyDpi(app->Platform.DpiScale, LOWORD(lp), HIWORD(lp));
            if (ctx->Backend != nullptr)
                ctx->Backend->Resize(LOWORD(lp), HIWORD(lp));
        }
        return 0;

    case WM_DPICHANGED:
        if (app != nullptr)
        {
            // Dragged to a monitor with a different scale factor. Take the
            // rect Windows suggests, then re-derive the logical size from it.
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            const float scale = static_cast<float>(LOWORD(wp)) / 96.0f;
            if (suggested != nullptr)
            {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RECT client {};
            GetClientRect(hwnd, &client);
            app->ApplyDpi(scale, client.right - client.left, client.bottom - client.top);
            if (ctx->Backend != nullptr)
                ctx->Backend->Resize(client.right - client.left, client.bottom - client.top);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (app != nullptr)
        {
            // Physical -> logical, to match the space the widget tree hit-tests in.
            const float inv = 1.0f / app->Platform.DpiScale;
            app->Platform.MousePos = ZSlate::Vector2(
                static_cast<float>(static_cast<short>(LOWORD(lp))) * inv,
                static_cast<float>(static_cast<short>(HIWORD(lp))) * inv);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (app != nullptr)
            app->Platform.MousePos = ZSlate::Vector2(-1.0f, -1.0f);
        return 0;

    case WM_LBUTTONDOWN: if (app) { app->Platform.MouseDown[0] = true;  SetCapture(hwnd); } return 0;
    case WM_LBUTTONUP:   if (app) { app->Platform.MouseDown[0] = false; ReleaseCapture(); } return 0;
    case WM_RBUTTONDOWN: if (app) app->Platform.MouseDown[1] = true;  return 0;
    case WM_RBUTTONUP:   if (app) app->Platform.MouseDown[1] = false; return 0;

    case WM_MOUSEWHEEL:
        if (app != nullptr)
        {
            // Accumulate: several wheel messages can arrive between frames.
            app->Platform.WheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        }
        return 0;

    case WM_KEYDOWN:
        if (app != nullptr)
        {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            ZSlate::SlateInputRouter::SetModifierState(ctrl, shift, alt);

            if (ctrl && wp == 'O')
            {
                app->OpenFileDialog();
                return 0;
            }
            // ZSlate's EKey has no page keys, so drive the table directly.
            if (wp == VK_PRIOR) { app->Table->ScrollByPage(-1); return 0; }
            if (wp == VK_NEXT)  { app->Table->ScrollByPage(+1); return 0; }

            const ZSlate::EKey key = MapVirtualKey(wp);
            if (key != ZSlate::EKey::Unknown)
                app->Input.ProcessKey(key);
        }
        return 0;

    case WM_CHAR:
        if (app != nullptr && wp >= 32)
            app->Input.ProcessChar(static_cast<unsigned int>(wp));
        return 0;

    case WM_DROPFILES:
        if (app != nullptr)
        {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            char path[MAX_PATH] = {};
            if (DragQueryFileA(drop, 0, path, MAX_PATH) > 0)
                app->LoadFile(path);
            DragFinish(drop);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
}  // namespace

int main(int argc, char** argv)
{
    const std::string mode = (argc >= 2) ? argv[1] : "";

    // ---- Headless UI verification -----------------------------------------
    // Paints one real frame into a recording renderer and asserts on what the
    // widgets emitted. Needs no GPU, no window and no font file, which is also
    // the only way to check virtualisation: a DXGI flip-model swapchain returns
    // a blank surface to PrintWindow, so a screenshot proves nothing.
    if (mode == "--dump-ui" && argc >= 3)
    {
        FPreviewApp app;
        ZSlate::SetPlatform(&app.Platform);
        app.BuildUI();
        app.LoadFile(argv[2]);
        if (!app.Snapshot.IsLoaded())
        {
            std::printf("ERROR: snapshot failed to load\n");
            return 1;
        }

        FSlateRecordingRenderer rec;
        app.Platform.RendererOverride = &rec;
        app.Platform.WinSize = ZSlate::Vector2(1440.0f, 900.0f);
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
        app.Platform.RendererOverride = &dpi;
        app.Platform.DpiScale = kTestScale;
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
        return (failed == 0) ? 0 : 4;
    }

    // ---- Headless stats ---------------------------------------------------
    // Keeps the data layer verifiable without a window, which is also what a
    // script or CI job wants when it just needs the per-type split.
    if (mode == "--stats" && argc >= 3)
    {
        FSnapshot snap;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!snap.LoadFromFile(argv[2], err))
        {
            std::printf("ERROR: %s\n", err.c_str());
            return 1;
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
            return 2;
        }
        return snap.IsSelfConsistent() ? 0 : 3;
    }

    // Before any window exists, and only on the interactive path -- the
    // headless modes above neither create a window nor care about DPI.
    DeclareDpiAwareness();

    FPreviewApp app;
    ZSlate::SetPlatform(&app.Platform);
    app.InitFonts();
    app.BuildUI();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "LuaMemPreviewWnd";
    RegisterClassA(&wc);

    // Scale the default window size by the primary monitor's factor, so the
    // 1440x900 logical default still occupies the intended physical area
    // instead of shrinking to 2/3 once virtualisation is off.
    float initial_scale = 1.0f;
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr)
        {
            const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
            if (dpi > 0)
                initial_scale = static_cast<float>(dpi) / 96.0f;
            ReleaseDC(nullptr, screen);
        }
    }

    FWinCtx ctx {&app, nullptr};
    HWND hwnd = CreateWindowA("LuaMemPreviewWnd", "Lua Memory Preview",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(app.Platform.WinSize.x * initial_scale),
                              static_cast<int>(app.Platform.WinSize.y * initial_scale),
                              nullptr, nullptr, GetModuleHandle(nullptr), &ctx);
    if (hwnd == nullptr)
    {
        std::printf("[LuaMemPreview] CreateWindow failed\n");
        return 1;
    }
    app.Hwnd = hwnd;

    // Per-window DPI is authoritative (multi-monitor); re-derive the logical
    // size from the real client rect now that the window exists.
    {
        RECT client {};
        GetClientRect(hwnd, &client);
        app.ApplyDpi(QueryDpiScaleForWindow(hwnd),
                     client.right - client.left, client.bottom - client.top);
    }
    DragAcceptFiles(hwnd, TRUE);

    auto* backend = new ZSlate::ZSlateD3D11Renderer(hwnd);
    ctx.Backend = backend;
    if (!backend->Init())
    {
        std::printf("[LuaMemPreview] D3D11 init failed\n");
        delete backend;
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    if (argc > 1)
        app.LoadFile(argv[1]);

    MSG msg = {};
    bool running = true;
    while (running)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running)
            break;

        app.RunFrame();
        backend->Render(app.Platform.Renderer, &app.FontAtlas);
        Sleep(1);
    }

    delete backend;  // destructor calls Shutdown(); do not call it twice
    return 0;
}
