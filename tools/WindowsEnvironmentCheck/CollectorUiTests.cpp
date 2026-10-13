#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Widgets/Input/SComboBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "CollectorScrollPanel.h"
#include "ZSlate/Renderer/SlateDpiScaleRenderer.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include "ZSlate/Testing/SlateRecordingRenderer.h"
#include <Windows.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace ZSlate;
namespace
{
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void TestScrollBar(float scale)
{
    struct Content : SWidget
    {
        Vector2 size;
        mutable int paints = 0;
        Vector2 ComputeDesiredSize() const override { return size; }
        void OnPaint(const FPaintContext& ctx, const FGeometry& geometry) const override
        {
            ++paints;
            ctx.Renderer->DrawQuad(geometry.ToRect(), Colors::White);
        }
    };
    auto content = std::make_shared<Content>(); content->size = Vector2(800 * scale, 1600 * scale);
    auto box = std::make_shared<SScrollBox>(); box->AddChild(content); box->ScrollSpeed = 40 * scale;
    auto bar = std::make_shared<SScrollBar>(); bar->Thickness = 16 * scale; bar->MinThumbLength = 32 * scale;
    box->SetScrollBar(bar);
    auto root = std::make_shared<zengine::envcheck::collector::CollectorScrollPanel>(box, bar);
    ZSlateBatchedRenderer renderer;
    auto paint = [&] {
        renderer.BeginFrame(); FPaintContext ctx; ctx.Renderer = &renderer;
        content->paints = 0;
        root->Paint(ctx, FGeometry(Vector2(0, 0), Vector2(600 * scale, 400 * scale)));
        renderer.EndFrame();
        Check(content->paints == 1, "scroll content drawn once");
        Check(box->GetCachedGeometry().ToRect().Right() <= bar->GetCachedGeometry().ToRect().x, "bar has its own right-hand lane");
        const auto& vertices = renderer.GetVertices();
        Check(vertices.size() >= 4 && vertices[2].Pos[0] <= bar->GetCachedGeometry().ToRect().x + .01f, "content clipped before scrollbar");
    };
    SlateInputRouter router;
    const float x = 592 * scale;
    auto mouse = [&](float px, float y, bool down, float wheel = 0) { router.ProcessMouse(root, Vector2(px, y), true, down, wheel); };
    paint();
    Check(std::abs(bar->ViewFraction - .25f) < .001f && bar->GetThumbRect().h == 100 * scale, "proportional thumb");
    mouse(x, 50 * scale, true);
    mouse(x + 80 * scale, 200 * scale, true);
    paint();
    Check(std::abs(box->ScrollOffset - 600 * scale) < .01f, "drag persists after paint even outside scrollbar");
    mouse(x + 80 * scale, 200 * scale, false);
    mouse(x, 300 * scale, false);
    Check(std::abs(box->ScrollOffset - 600 * scale) < .01f, "release ends drag");
    mouse(x, 300 * scale, false, 1); paint();
    Check(std::abs(box->ScrollOffset - 560 * scale) < .01f, "bar wheel moves one line");
    mouse(20 * scale, 100 * scale, false, 1); paint();
    Check(std::abs(box->ScrollOffset - 520 * scale) < .01f, "content wheel uses same speed");
    box->ScrollOffset = 0; paint();
    mouse(x, 350 * scale, true); mouse(x, 350 * scale, false); paint();
    Check(std::abs(box->ScrollOffset - 400 * scale) < .01f, "track click pages content");
    box->ScrollOffset = 0; paint();
    mouse(x, 50 * scale, true); router.ResetPointerState();
    bar->OnMouseMove(Vector2(x, 350 * scale)); paint();
    Check(box->ScrollOffset == 0, "capture loss stops thumb drag");
    router.Reset();
    mouse(x, 50 * scale, true); mouse(x, 800 * scale, true); mouse(x, 800 * scale, false); paint();
    Check(bar->Value == 1 && box->ScrollOffset == 1200 * scale, "drag clamps to bottom");
    content->size.y = 100 * scale; paint();
    Check(box->ScrollOffset == 0 && bar->Value == 0 && bar->ViewFraction == 1, "resize to fitting content resets range");
    router.Reset(); mouse(x, 50 * scale, true); mouse(x, 300 * scale, true); mouse(x, 300 * scale, false);
    Check(box->ScrollOffset == 0, "fitting content does not scroll");
    renderer.BeginFrame(); FPaintContext ctx; ctx.Renderer = &renderer;
    bar->ViewFraction = .01f;
    bar->Paint(ctx, FGeometry(Vector2(0, 0), Vector2(16 * scale, 8 * scale)));
    renderer.EndFrame();
    Check(bar->GetThumbRect().h <= 8 * scale, "minimum thumb never exceeds short track");
    std::weak_ptr<SScrollBox> lifetime = box;
    root.reset(); box.reset();
    Check(lifetime.expired(), "scroll callback does not retain box");
    bar->OnMouseWheel(Vector2(0, 0), -1);
}
void TestPointerReset()
{
    auto input = std::make_shared<SEditableTextBox>();
    FPaintContext context;
    input->Paint(context, FGeometry(Vector2(0, 0), Vector2(300, 40)));
    SlateInputRouter router;
    router.ProcessMouse(input, Vector2(10, 10), true, true, 0);
    router.ProcessMouse(input, Vector2(10, 10), true, false, 0);
    router.ResetPointerState();
    router.ProcessChar('a');
    Check(input->IsFocused() && input->GetText() == "a", "DPI pointer reset preserves focused input");
    router.Reset();
    Check(!input->IsFocused() && !router.HasKeyboardFocus(), "full reset clears visible and routed focus");
    struct Button : SButton
    {
        int lost = 0;
        void OnMouseCaptureLost() override { ++lost; SButton::OnMouseCaptureLost(); }
    };
    auto button = std::make_shared<Button>();
    button->Paint(context, FGeometry(Vector2(0, 0), Vector2(300, 40)));
    router.ProcessMouse(button, Vector2(10, 10), true, true, 0);
    router.Reset();
    Check(button->lost == 1, "reset notifies captured button");
}
void TestDropdown(float scale)
{
    auto combo = std::make_shared<SComboBox<int>>();
    combo->Options = {0, 1, 2};
    combo->SelectedIndex = 0;
    combo->ExternalPopup = true;
    combo->ItemHeight = 32 * scale;
    combo->MaxDropdownHeight = 96 * scale;
    combo->OnGetLabel = [](int value) { return std::to_string(value); };
    int changes = 0;
    combo->OnSelectionChanged = [&](int, int32_t) { ++changes; };
    ZSlateBatchedRenderer renderer;
    const UIRect viewport(0, 0, 600 * scale, 400 * scale);
    auto paint = [&](float y = 20) {
        renderer.BeginFrame();
        FPaintContext context; context.Renderer = &renderer;
        combo->Paint(context, FGeometry(Vector2(20 * scale, y * scale), Vector2(260 * scale, 32 * scale)));
        combo->PaintPopup(&renderer, viewport);
        renderer.EndFrame();
    };
    SlateInputRouter router;
    paint();
    router.ProcessMouse(combo, Vector2(40 * scale, 30 * scale), true, true, 0);
    Check(combo->IsOpen(), "click opens dropdown through router");
    paint();
    combo->ProcessPopupMouse(Vector2(40 * scale, 30 * scale), false, 0);
    combo->OnMouseLeave();
    Check(combo->IsOpen(), "leaving anchor keeps menu open");
    const Vector2 item(40 * scale, (52 + 48) * scale);
    Check(combo->ProcessPopupMouse(item, true, 0), "popup captures press");
    Check(combo->ProcessPopupMouse(item, false, 0), "popup captures release");
    Check(!combo->IsOpen() && combo->SelectedItem == 1 && changes == 1, "second item selected once");
    combo->OnKeyDown(EKey::Enter); paint();
    Check(combo->ProcessPopupMouse(Vector2(500 * scale, 250 * scale), true, 0), "outside click consumed");
    Check(!combo->IsOpen() && changes == 1, "outside dismiss without selection");
    combo->OnKeyDown(EKey::Enter); paint();
    combo->OnKeyDown(EKey::Down);
    combo->OnKeyDown(EKey::Escape);
    Check(combo->SelectedItem == 1 && changes == 1, "escape cancels keyboard preview");
    combo->OnKeyDown(EKey::Enter); paint();
    combo->OnKeyDown(EKey::Down);
    combo->OnKeyDown(EKey::Enter);
    Check(combo->SelectedItem == 2 && changes == 2, "keyboard selection");
    combo->OnKeyDown(EKey::Enter); paint(360);
    combo->ProcessPopupMouse(Vector2(40 * scale, 280 * scale), true, 0);
    combo->ProcessPopupMouse(Vector2(40 * scale, 280 * scale), false, 0);
    Check(!combo->IsOpen() && combo->SelectedItem == 0, "dropdown flips above near viewport bottom");
    combo->OnKeyDown(EKey::Enter);
    combo->OnFocusLost();
    Check(!combo->IsOpen(), "focus loss closes popup");
}
void TestGlyphMapping()
{
    char windows[MAX_PATH]{};
    Check(GetWindowsDirectoryA(windows, MAX_PATH) != 0, "Windows fonts directory");
    ZSlateFontAtlas atlas;
    const auto fonts = std::filesystem::path(windows) / "Fonts";
    Check(atlas.LoadFromFile((fonts / "segoeui.ttf").string()), "load UI font");
    Check(atlas.AddFallbackFont((fonts / "msyh.ttc").string()) || atlas.AddFallbackFont((fonts / "simsun.ttc").string()), "load Chinese font");
    ZSlateBatchedRenderer renderer;
    renderer.SetFontAtlas(&atlas);
    for (float dpiScale : {1.0f, 1.25f, 1.5f, 2.0f})
    for (float offset : {0.0f, 0.2f, 0.49f, 0.5f, 0.8f})
    for (bool clipped : {false, true})
    {
        renderer.BeginFrame();
        if (clipped) renderer.PushClipRect(UIRect(16.4f, 21.3f, 190.7f, 28.9f));
        renderer.DrawTextLabel(UIRect(10 + offset, 10 + offset, 800, 90), "环境信息 闪退 卡住 AVmw 0123456789", 18 * dpiScale, Colors::White);
        if (clipped) renderer.PopClipRect();
        renderer.EndFrame();
        const auto& vertices = renderer.GetVertices();
        Check(!vertices.empty() && vertices.size() % 4 == 0, "glyph quads generated");
        for (size_t i = 0; i < vertices.size(); i += 4)
        {
            const auto& a = vertices[i]; const auto& b = vertices[i + 2];
            Check(std::abs((b.Pos[0] - a.Pos[0]) - (b.UV[0] - a.UV[0]) * atlas.GetWidth()) < 0.002f, "glyph texel-to-pixel width must be 1:1");
            Check(std::abs((b.Pos[1] - a.Pos[1]) - (b.UV[1] - a.UV[1]) * atlas.GetHeight()) < 0.002f, "glyph texel-to-pixel height must be 1:1");
        }
    }
    auto fractional = renderer.MeasureText("文本 Measure", 22.5f);
    auto quantized = renderer.MeasureText("文本 Measure", 23.0f);
    Check(fractional.x == quantized.x && fractional.y == quantized.y, "measure and raster size quantization agrees");

    // The GUI paints text through FSlateDpiScaleRenderer (logical 18pt -> physical
    // 18*s), so the 1:1 texel mapping must survive the decorator too.
    for (float dpiScale : {1.25f, 1.5f, 1.75f, 2.0f})
    for (float offset : {0.0f, 0.3f, 0.5f, 0.7f})
    {
        FSlateDpiScaleRenderer dpi(&renderer, dpiScale, true);
        dpi.BeginFrame();
        dpi.DrawTextLabel(UIRect(10 + offset, 10 + offset, 640, 60), "环境信息 闪退 卡住 AVmw 0123456789", 18, Colors::White);
        dpi.EndFrame();
        const auto& vertices = renderer.GetVertices();
        Check(!vertices.empty(), "decorated glyph quads generated");
        for (size_t i = 0; i < vertices.size(); i += 4)
        {
            const auto& a = vertices[i]; const auto& b = vertices[i + 2];
            Check(std::abs((b.Pos[0] - a.Pos[0]) - (b.UV[0] - a.UV[0]) * atlas.GetWidth()) < 0.002f, "decorated glyph width stays 1:1");
            Check(std::abs((b.Pos[1] - a.Pos[1]) - (b.UV[1] - a.UV[1]) * atlas.GetHeight()) < 0.002f, "decorated glyph height stays 1:1");
            Check(a.Pos[0] == std::floor(a.Pos[0]) && a.Pos[1] == std::floor(a.Pos[1]), "decorated glyph origin on pixel grid");
        }
    }
}
// Records DrawRect thickness as well, which FSlateRecordingRenderer only counts.
struct RectRecorder : FSlateRecordingRenderer
{
    std::vector<UIRect> rects;
    std::vector<float> thickness;
    void DrawRect(const UIRect& rect, const UIColor& color, float t) override
    {
        FSlateRecordingRenderer::DrawRect(rect, color, t);
        rects.push_back(rect); thickness.push_back(t);
    }
};
bool IsWhole(float v) { return v == std::floor(v); }
void TestDpiPixelSnap()
{
    // Logical row of boxes that tile the line with no gaps -- the layout the GUI
    // produces (22px side padding, integer widths). Fractional scales put most
    // of these edges between physical pixels.
    const float widths[] = {22, 7, 13, 260, 1, 3, 560, 22};
    for (float s : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f})
    {
        RectRecorder rec;
        FSlateDpiScaleRenderer dpi(&rec, s, true);
        float x = 0;
        for (float w : widths) { dpi.DrawQuad(UIRect(x, 5, w, 32), Colors::White); x += w; }
        Check(rec.Quads.size() == std::size(widths), "every quad forwarded");
        for (size_t i = 0; i < rec.Quads.size(); ++i)
        {
            const UIRect& q = rec.Quads[i];
            Check(IsWhole(q.x) && IsWhole(q.y) && IsWhole(q.w) && IsWhole(q.h), "snapped quad on pixel grid");
            Check(q.w >= 1 && q.h >= 1, "snapped quad never collapses");
            if (i + 1 < rec.Quads.size())
                Check(q.x + q.w == rec.Quads[i + 1].x, "adjacent quads share an edge (no seam/overlap)");
        }
        // Edge snapping keeps total extent faithful: at most half a pixel of drift.
        const UIRect& last = rec.Quads.back();
        Check(std::abs((last.x + last.w) - x * s) <= 0.5f, "row extent within half a pixel");

        // Hairlines: a sub-pixel quad still shows, a border is whole pixels >= 1.
        dpi.DrawQuad(UIRect(10.3f, 10, 0.2f, 0.2f), Colors::White);
        Check(rec.Quads.back().w == 1 && rec.Quads.back().h == 1, "sub-pixel quad rounds up to 1px");
        dpi.DrawQuad(UIRect(10, 10, 0, 4), Colors::White);
        Check(rec.Quads.back().w == 0, "empty quad stays empty");
        dpi.DrawRect(UIRect(0, 0, 100, 20), Colors::White, 1.0f);
        Check(IsWhole(rec.thickness.back()) && rec.thickness.back() >= 1, "border thickness whole pixels");
        Check(std::abs(rec.thickness.back() - s) <= 0.5f, "border thickness tracks scale");

        // Clip rects snap with the same rule, so content and its clip agree.
        dpi.PushClipRect(UIRect(22, 0, 260, 400));
        dpi.DrawQuad(UIRect(22, 0, 260, 400), Colors::White);
        dpi.PopClipRect();
        Check(rec.IsClipBalanced(), "clip stack balanced through decorator");

        // Text keeps the exact scaled size (atlas does its own rounding).
        dpi.DrawTextLabel(UIRect(22, 5, 100, 20), "x", 17, Colors::White);
        Check(rec.Texts.back().FontSize == 17 * s, "font size scaled, not snapped");
        // MeasureText round-trips to logical units.
        const Vector2 m = dpi.MeasureText("MMMM", 14);
        Check(std::abs(m.x - rec.MeasureText("MMMM", 14).x) < 0.01f, "measure returns logical units");
    }

    // Snap off: an exact similarity transform (what LuaMemPreview asserts).
    RectRecorder rec;
    FSlateDpiScaleRenderer exact(&rec, 1.25f);
    exact.DrawQuad(UIRect(22, 7, 13, 5), Colors::White);
    const UIRect& q = rec.Quads.back();
    Check(q.x == 27.5f && q.y == 8.75f && q.w == 16.25f && q.h == 6.25f, "unsnapped decorator is exact");
}
}
int main()
{
    try
    {
        TestPointerReset();
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) { TestDropdown(scale); TestScrollBar(scale); }
        TestGlyphMapping();
        TestDpiPixelSnap();
        std::cout << "PASS dropdown, scrollbar drag/wheel/page/capture/clipping, 1:1 glyph mapping and DPI pixel snapping at 100/125/150/200 percent\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
