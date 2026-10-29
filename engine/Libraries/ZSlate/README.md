# ZSlate

A lightweight, retained-mode UI framework for C++20 applications.

## Features

- **Retained-mode widget tree** — Declarative UI construction with automatic layout
- **Rich widget library** — 40+ built-in widgets covering text, input, layout, views,
  panels, indicators, and advanced controls — 100% UE Slate core widget parity
- **Batch renderer** — `ZSlateBatchedRenderer` records CPU draw commands that any GPU
  backend can consume
- **D3D11 renderer** — Optional `ZSlateD3D11Renderer` GPU backend (zero-config)
- **Font atlas** — TTF/OTC glyph rasterization via `ZSlateFontAtlas` + `ZSlateTextGenerator`
  (stb_truetype, on-demand baking, CJK fallback chain)
- **C++20 ready** — Uses modern C++ features (lambdas, etc.)
- **Header-friendly** — Minimal dependencies

## Architecture

```
ZSlate/
├── Core/            # Types (Vector2, UIRect, UIColor), geometry, enums,
│                    # SlateLog (diagnostics sink),
│                    # SlateLocalization (language switch + string lookup)
├── Application/     # SlateApplication, SlateInputRouter (mouse/keyboard)
├── Renderer/        # ZSlateBatchedRenderer (CPU batch recorder),
│                    # ZSlateFontAtlas (TTF→atlas), ZSlateTextGenerator (layout)
├── Platform/
│   └── D3D11/       # ZSlateD3D11Renderer (optional GPU backend)
├── Backend/         # ZEngineSlateRenderer (bridges to ZEngine runtime)
├── Widgets/
│   ├── Text/        # STextBlock, SRichTextBlock, SMultiLineEditableText
│   ├── Input/       # SButton, SCheckBox, SSlider, SEditableTextBox,
│   │                 # SComboBox, SSpinBox, SSearchBox, SNumericEntryBox
│   ├── Layout/      # SBoxPanel, SScrollBox, SSplitter, SSpacer,
│   │                 # SGridPanel, SWrapBox, SScrollBar, SScaleBox,
│   │                 # SUniformGridPanel, SCanvas
│   ├── Views/       # SListView, STreeView, STableViewBase
│   ├── Panels/      # SBorder, SOverlay, SImage
│   ├── SProgressBar, SThrobber, SNotificationList
│   ├── SColorPicker, SExpandableArea, SHeaderRow
│   ├── SMenu, SDragFloat, SDropTarget, SWindowTitleBar
│   ├── SWidget.h    # Base widget class
│   ├── SLeafWidget.h
│   ├── SCompoundWidget.h
│   └── SPanel.h     # Slot-based panel base
├── thirdparty/      # zstb_truetype.h (stb_truetype 1.26)
└── examples/        # Standalone demo apps
```

## Quick Start

### Option 1: Use D3D11 backend (recommended for tools)

```cpp
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

// --- 1. Platform stub ---
struct MyPlatform : public ZSlate::ISlatePlatform
{
    ZSlate::ZSlateBatchedRenderer Renderer;
    ZSlate::Vector2 m_MousePos, m_WinSize {1024, 768};

    ZSlate::ISlateRenderer* GetRenderer()    override { return &Renderer; }
    ZSlate::Vector2 GetMousePosition() const override { return m_MousePos; }
    bool IsMouseButtonDown(int)         const override { return false; }
    bool IsKeyDown(int)                 const override { return false; }
    float GetTimeSeconds()              const override { return 0.0f; }
    ZSlate::Vector2 GetWindowSize()     const override { return m_WinSize; }
};

// --- 2. WinMain ---
int WINAPI WinMain(...)
{
    // Font atlas (text rendering)
    ZSlate::ZSlateFontAtlas fontAtlas;
    fontAtlas.LoadFromFile("C:\\Windows\\Fonts\\segoeui.ttf");
    fontAtlas.AddFallbackFont("C:\\Windows\\Fonts\\msyh.ttc");  // CJK

    MyPlatform platform;
    platform.Renderer.SetFontAtlas(&fontAtlas);
    ZSlate::SetPlatform(&platform);

    // Build UI
    auto root = std::make_shared<ZSlate::SVerticalBox>();
    auto title = std::make_shared<ZSlate::STextBlock>();
    title->Text = "Hello ZSlate!";
    root->AddSlot(title);

    ZSlate::SlateApplication::Get().SetRootContent(root);

    // Create window + D3D11 backend
    HWND hwnd = CreateWindowA(...);
    ZSlate::ZSlateD3D11Renderer backend(hwnd);
    backend.Init();

    // Main loop
    while (running) {
        platform.Renderer.BeginFrame();
        ZSlate::SlateApplication::Get().PaintInto(&platform.Renderer,
            ZSlate::UIRect(0, 0, 1024, 768));
        platform.Renderer.EndFrame();
        backend.Render(platform.Renderer, &fontAtlas);
    }

    backend.Shutdown();
}
```

### Option 2: Implement ISlateRenderer directly

```cpp
struct MyRenderer : public ZSlate::ISlateRenderer
{
    void DrawQuad(const UIRect& rect, const UIColor& color) override { /* ... */ }
    void DrawRect(const UIRect& rect, const UIColor& color, float t) override { /* ... */ }
    void DrawTexturedQuad(const UIRect& rect, void* tex, const UIColor& tint) override { /* ... */ }
    void DrawTextLabel(const UIRect& rect, const std::string& text, float size,
                       const UIColor& color, TextAnchor anchor, TextWrapMode wrap,
                       void* font_handle) override { /* ... */ }
    void DrawTextLabel(const std::string& text, const Vector2& pos, float size,
                       const UIColor& color) override { /* ... */ }
    Vector2 MeasureText(const std::string& text, float size) const override { /* ... */ }
    // ... PushClipRect, PopClipRect, BeginFrame, EndFrame
};
```

> **Note on naming**: `DrawTextLabel` was intentionally chosen over `DrawText` to avoid
> collision with the `windows.h` `DrawText` macro (which `#define`s `DrawText` →
> `DrawTextA`/`DrawTextW`).

### Option 0 (preferred): FSlateStandaloneApp — the whole host, done for you

Options 1 and 2 above are what every standalone ZSlate tool used to re-implement
by hand, five times over: window class, message pump, `ISlatePlatform`, DPI
awareness + `WM_DPICHANGED`, and a hardcoded font load. The copies drifted —
two had no DPI support at all, four hardcoded `C:\Windows\Fonts`, and the DPI
decorator was constructed with different pixel-snapping flags.

`ZSlate/App/SlateStandaloneApp.h` owns all of it. **New tools should start here.**

```cpp
#include "ZSlate/App/SlateStandaloneApp.h"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    ZSlate::FStandaloneAppDesc desc;
    desc.Title  = "My Tool";
    desc.Width  = 1280.0f;              // logical (96-DPI) units
    desc.Height = 800.0f;
    desc.Font   = ZSlate::Win32::ESystemFont::Mono;   // or ::Sans

    ZSlate::FSlateStandaloneApp app;
    if (!app.Initialize(desc)) return 1;   // DPI awareness, fonts, window, D3D11

    app.SetRootContent(BuildMyUI());

    app.OnDropFile = [](const std::string& p) { /* ... */ return true; };
    app.OnKey      = [](unsigned vk, bool down) -> bool { /* true = consumed */ return false; };

    return app.Run();                      // pump + TickFrame + Present
}
```

You get, with no extra code:

- **Per-monitor DPI v2** declared before the first window exists, `WM_DPICHANGED`
  handled, and an initial window sized for the monitor scale.
- **Fonts**: Segoe UI / Consolas + a CJK fallback chain, located via
  `GetWindowsDirectoryW()` — never a hardcoded drive letter.
- **Logical/physical coordinate conversion** in exactly one place: the WndProc
  divides on the way in, `FSlateDpiScaleRenderer` multiplies on the way out.
  Widgets keep authoring in 96-DPI units.
- **UTF-16 surrogate pairing** on `WM_CHAR`, mouse capture, focus-loss button
  reset, clipboard, and a text measurer wired through the DPI decorator.

Supporting helpers, usable without the full shell:

| Header | Contents |
|---|---|
| `ZSlate/Platform/Win32/SlateWin32Dpi.h` | `DeclareDpiAwareness`, `QueryDpiScaleForWindow`, `QuerySystemDpiScale`, `ResizeWindowForDpi` |
| `ZSlate/Platform/Win32/SlateWin32Host.h` | `LoadSystemFonts`, `KeyFromVirtualKey`, `InstallClipboardBridge`, `Utf8FromWide`/`WideFromUtf8`, `CommandLineArgsUtf8` |

The console helpers that used to be here (`EnsureConsole` / `StdOutIsRedirected`)
are now `zengine::appshell::EnsureConsole()` in `ZAppShell/AppMain.h`, shared
with the rest of the engine. It is header-only on purpose: ZSlate is `/MD` like
the tree, while `ZEnvironmentCollector` ships `/MT`, so a version compiled into
this library was unusable by one of the two and had been copied. `CommandLineArgsUtf8`
stays for ZSlate's own users; inside ZEngine use
`zengine::appshell::CommandLine` so every executable reads the same wide source.

Both are Windows-only and compiled out on other platforms, like
`Platform/D3D11/ZSlateD3D11Renderer.h`.

### Tests

```bash
cmake -S . -B build -DZSLATE_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

`tests/slate_list_view_tests.cpp` is the pattern to follow: headless (no GPU,
window or font file), driven through `FSlateRecordingRenderer`, asserting on
**draw-call counts** rather than pixels — a screenshot cannot show whether list
virtualisation works, because a DXGI flip-model swapchain hands GDI a blank
surface. It currently pins `SListView` at 180K rows: bounded text draws, bounded
live widgets, recycled rows carrying fresh data, and a balanced clip stack.

## CMake Integration

```cmake
add_subdirectory(${ZSLATE_PATH}/ZSlate ${CMAKE_CURRENT_BINARY_DIR}/ZSlate)
target_link_libraries(MyApp PRIVATE ZSlate)
```

## Widget Reference

### Text Widgets (`Widgets/Text/`)

| Widget | Description |
|--------|-------------|
| `STextBlock` | Single line text display |
| `SRichTextBlock` | Rich text display with inline formatting (bold, color, links, etc.) |
| `SMultiLineEditableText` | Multi-line text editor with selection, clipboard, scroll |

### Input Widgets (`Widgets/Input/`)

| Widget | Description |
|--------|-------------|
| `SButton` | Clickable button |
| `SCheckBox` | Check box toggle |
| `SSlider` | Slider control |
| `SEditableTextBox` | Single-line text input |
| `SComboBox<T>` | Dropdown selector |
| `SSpinBox` | Numeric spinner (+/- buttons + drag) |

### Layout Widgets (`Widgets/Layout/`)

| Widget | Description |
|--------|-------------|
| `SBoxPanel` | Horizontal/Vertical box layout |
| `SScrollBox` | Scrollable container (with SScrollBar sync) |
| `SScrollBar` | Vertical/horizontal scroll bar |
| `SSplitter` | Resizable splitter |
| `SSpacer` | Empty space |
| `SGridPanel` | Uniform grid (N×M) |
| `SWrapBox` | Flowing wrap layout |

### Views Widgets (`Widgets/Views/`)

| Widget | Description |
|--------|-------------|
| `SListView<T>` | Virtualized list view |
| `STreeView<T>` | Hierarchical tree with expand/collapse |

### Panels Widgets (`Widgets/Panels/`)

| Widget | Description |
|--------|-------------|
| `SBorder` | Decorative border container |
| `SOverlay` | Z-ordered overlay (Z-Stack) |
| `SImage` | Image display |

### Other Widgets

| Widget | Description |
|--------|-------------|
| `SWidget` | Base widget class |
| `SLeafWidget` | Leaf widget base (no children) |
| `SCompoundWidget` | Single-child wrapper |
| `SMenu` | Menu system |
| `SProgressBar` | Progress bar (0..1 fill) |
| `SThrobber` | Animated loading indicator |

### SRichTextBlock Example

```cpp
#include "ZSlate/Widgets/Text/SRichTextBlock.h"

auto RichText = ZSlate::CreateRichTextBlock(
    "This is <b>bold</b>, <i>italic</i>, <u>underline</u>, "
    "and <color=#FF0000>red text</color>.\n"
    "Visit <Hyperlink url=\"https://example.com\">our website</Hyperlink>.",
    ZSlate::SRichTextBlock::FRichTextOptions{
        .FontSize = 14.0f,
        .Color = ZSlate::Colors::White,
        .AutoWrapText = true
    }
);

RichText->SetOnHyperlinkNavigate([](const std::string& Url) {
    std::cout << "Navigating to: " << Url << std::endl;
});
```

**Supported Tags:** `<b>`, `<strong>`, `<i>`, `<em>`, `<u>`, `<s>`, `<strike>`,
`<color=#RRGGBB>`, `<size=N>`, `<Hyperlink url="...">text</>`, `<br>`

### SListView Example

```cpp
#include "ZSlate/Widgets/Views/SListView.h"

struct FMyItem { std::string Name; int32 Value; };

std::vector<FMyItem> Items;
for (int i = 0; i < 1000; ++i)
    Items.push_back({"Item " + std::to_string(i), i});

auto ListView = ZSlate::CreateListView<FMyItem>(
    std::move(Items),
    [](const FMyItem& Item, int32 Index, auto& L) {
        auto Row = std::make_shared<ZSlate::SHorizontalBox>();
        auto NameText = std::make_shared<ZSlate::STextBlock>();
        NameText->Text = "[" + std::to_string(Index) + "] " + Item.Name;
        NameText->FontSize = 14.0f;
        Row->AddSlot(NameText);
        return Row;
    },
    ZSlate::SListView<FMyItem>::FListViewOptions{.ItemHeight = 24.0f}
);
```

## Type Reference

```cpp
// Vector2 — 2D vector
struct Vector2 { float x, y; };

// UIRect — Rectangle
struct UIRect { float x, y, w, h; };

// UIColor — RGBA color (0.0–1.0)
using UIColor = Vector4;
namespace Colors { constexpr UIColor White, Black, Red, ... };
```

## Font Atlas (ZSlateFontAtlas)

```cpp
#include "ZSlate/Renderer/ZSlateFontAtlas.h"

ZSlate::ZSlateFontAtlas atlas;
atlas.LoadFromFile("C:\\Windows\\Fonts\\segoeui.ttf");   // primary
atlas.AddFallbackFont("C:\\Windows\\Fonts\\msyh.ttc");    // CJK fallback

renderer.SetFontAtlas(&atlas);  // attach to batch renderer
// Font atlas auto-uploads to GPU via ZSlateD3D11Renderer::UploadFontAtlas()
```

## Localization (ZSlate/Core/SlateLocalization.h)

ZSlate owns the **mechanism**, not the translations. English is the text written
in the source; every other language is a table the host registers. A tool that
wants a Chinese UI therefore writes English and ships a table -- it does not
fork its strings.

```cpp
#include "ZSlate/Core/SlateLocalization.h"

// 1. Write strings in English, tagged with a stable id.
label->SetText(ZSlate::Loc("shell.tab.collection", "Collection"));

// 2. Register translations once, before building the widget tree.
const ZSlate::FLocalizationEntry zh[] = {{"shell.tab.collection", "环境采集"}};
ZSlate::RegisterStrings(ZSlate::ELanguage::ChineseSimplified, zh, std::size(zh));

// 3. Switch at run time and remember the choice however the host likes.
ZSlate::SetLanguage(ZSlate::ELanguage::ChineseSimplified);
SaveSetting("lang", ZSlate::LanguageId(ZSlate::GetLanguage()));   // "zh-CN"
```

An id with no entry falls back to the English literal (and reports the miss once
through `ZSlate::Log`), so a half-translated build degrades instead of showing
blank labels. Strings are resolved when a widget is **built**, so switching
language means rebuilding the tree, not repainting.

## Platform Implementations

- **ZSlateD3D11Renderer** — D3D11 GPU backend, self-contained (no ZEngine dep)
- **ZEngineSlateRenderer** — Bridges to ZEngine's `BatchedUIRenderer` + UIPass
- **MockRenderer** — Console output for testing (see `examples/`)

## License

See the project LICENSE for details.
