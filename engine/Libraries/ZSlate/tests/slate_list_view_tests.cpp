// ============================================================================
// SListView virtualisation + recycling regression tests (headless)
// ============================================================================
// These assertions exist because LuaMemPreview/LuaMemWidgets.h used to carry a
// comment stating that SListView was unusable at ~180K rows for two reasons:
//
//   1. GetOrCreateWidget() only called OnGenerateRow when the pool was empty,
//      so a recycled widget kept the previous row's data -> scrolling showed
//      wrong values.
//   2. ReleaseInvisibleWidgets() was never called from OnPaint -> the
//      index -> widget map grew monotonically and retained a widget per row.
//
// Both are fixed (the map is keyed by index, released off-screen, and cleared
// whenever the data source revision changes), but nothing proved it, so the
// stale comment kept being cited as the reason to hand-paint list widgets.
// These tests pin the behaviour down; if they fail, the comment becomes true
// again and must be restored.
//
// Headless on purpose: no GPU, no window, no font file. FSlateRecordingRenderer
// counts draw calls, which is the only way to see "how many rows were painted"
// -- a screenshot cannot show it (a DXGI flip-model swapchain hands GDI a
// blank surface).

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Testing/SlateRecordingRenderer.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Views/SListView.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{

// A platform whose renderer is the recorder, sized so that ~25 rows fit.
class FTestPlatform : public ZSlate::ISlatePlatform
{
public:
    explicit FTestPlatform(ZSlate::ISlateRenderer* renderer) : m_Renderer(renderer) {}

    ZSlate::ISlateRenderer* GetRenderer() override { return m_Renderer; }
    ZSlate::Vector2 GetMousePosition() const override { return ZSlate::Vector2(-1.0f, -1.0f); }
    bool IsMouseButtonDown(int) const override { return false; }
    bool IsKeyDown(int) const override { return false; }
    float GetTimeSeconds() const override
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
    ZSlate::Vector2 GetWindowSize() const override { return ZSlate::Vector2(800.0f, 600.0f); }

private:
    ZSlate::ISlateRenderer* m_Renderer;
};

// Bracketed so the labels cannot substring-match each other: "[1]" is not
// contained in "[179999]", which plain "item 1" / "item 179999" would be.
std::string Label(int index) { return "[" + std::to_string(index) + "]"; }

struct FCheck
{
    const char* Name;
    bool Ok;
};

}  // namespace

int main()
{
    // 180K rows: the Lua snapshot scale that motivated the hand-painted table.
    constexpr int kItems = 180000;
    constexpr float kItemHeight = 24.0f;
    constexpr float kViewW = 800.0f;
    constexpr float kViewH = 600.0f;

    // Rows that fit, plus the +2 partial-visibility overdraw SListView paints.
    const int kExpectedRows = static_cast<int>(kViewH / kItemHeight) + 2;
    const size_t kTextBudget = static_cast<size_t>(kExpectedRows) * 3;  // slack, still O(visible)

    ZSlate::FSlateRecordingRenderer rec;
    FTestPlatform platform(&rec);
    ZSlate::SetPlatform(&platform);

    auto view = std::make_shared<ZSlate::SListView<int>>();
    view->ItemHeight = kItemHeight;
    view->SelectionMode = ZSlate::STableViewBase::ESelectionMode::Single;
    view->OnGenerateRow = [](const int& item, int32_t)
    {
        auto text = std::make_shared<ZSlate::STextBlock>();
        text->Text = Label(item);
        text->FontSize = 12.0f;
        return text;
    };

    std::vector<int> items(kItems);
    for (int i = 0; i < kItems; ++i) items[i] = i;
    view->Items = std::move(items);

    ZSlate::SlateApplication::Get().SetRootContent(view);

    const ZSlate::UIRect region(0.0f, 0.0f, kViewW, kViewH);
    auto paint = [&]
    {
        rec.Reset();
        rec.BeginFrame();
        ZSlate::SlateApplication::Get().PaintInto(&rec, region);
        rec.EndFrame();
    };

    // ---- Frame 1: at the top ------------------------------------------------
    paint();
    const size_t texts_top = rec.Texts.size();
    const bool top_correct = rec.HasText(Label(0)) && rec.HasText(Label(20));
    const bool no_far_rows = !rec.HasText(Label(100000));
    const size_t live_top = view->GetLiveWidgetCount();

    // ---- Frame 2: scrolled to the bottom ------------------------------------
    // The recycling bug would show up here as "[0]" still on screen.
    view->ScrollToBottom(kItems, kViewH);
    paint();
    const bool bottom_correct = rec.HasText(Label(kItems - 1)) && rec.HasText(Label(kItems - 20));
    const bool no_stale_top = !rec.HasText(Label(0));
    const size_t live_bottom = view->GetLiveWidgetCount();

    // ---- Sweep: many scroll steps, widget count must stay bounded -----------
    size_t max_live = 0;
    size_t max_texts = 0;
    bool clip_always_balanced = rec.IsClipBalanced();
    for (int step = 0; step < 200; ++step)
    {
        view->SetScrollOffset(static_cast<float>(step) * kItemHeight * 7.0f);
        paint();
        if (view->GetLiveWidgetCount() > max_live) max_live = view->GetLiveWidgetCount();
        if (rec.Texts.size() > max_texts) max_texts = rec.Texts.size();
        if (!rec.IsClipBalanced()) clip_always_balanced = false;
    }

    // ---- Back to the top: rows must be regenerated, not blank ---------------
    view->SetScrollOffset(0.0f);
    paint();
    const bool top_after_roundtrip = rec.HasText(Label(0));

    const FCheck checks[] = {
        {"virtualised: text draws << item count", texts_top < kTextBudget},
        {"virtualised: widgets << item count",    live_top < kTextBudget},
        {"top renders the first rows",            top_correct},
        {"top does not render far rows",          no_far_rows},
        {"bottom renders the last rows",          bottom_correct},
        {"recycled rows carry fresh data",        no_stale_top},
        {"sweep: widgets stay bounded",           max_live < kTextBudget},
        {"sweep: text draws stay bounded",        max_texts < kTextBudget},
        {"sweep: clip stack always balanced",     clip_always_balanced},
        {"scrolling back regenerates row 0",      top_after_roundtrip},
    };

    int failures = 0;
    for (const FCheck& c : checks)
    {
        std::printf("  %s  %s\n", c.Ok ? "PASS" : "FAIL", c.Name);
        if (!c.Ok) ++failures;
    }

    std::printf("\n%d items, %d visible rows, %.0f px viewport\n", kItems, kExpectedRows, kViewH);
    std::printf("texts: %zu at top, %zu worst during sweep\n", texts_top, max_texts);
    std::printf("live widgets: %zu at top, %zu at bottom, %zu worst during sweep\n",
                live_top, live_bottom, max_live);

    ZSlate::SlateApplication::Get().SetRootContent(nullptr);
    ZSlate::SetPlatform(nullptr);

    if (failures != 0)
    {
        std::printf("\nFAILED (%d)\n", failures);
        return 1;
    }
    std::printf("\nPASS SListView virtualisation, recycling and clipping\n");
    return 0;
}
