#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Widgets/Input/SComboBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"
#include <Windows.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace ZSlate;
namespace
{
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
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
}
}
int main()
{
    try
    {
        TestPointerReset();
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) TestDropdown(scale);
        TestGlyphMapping();
        std::cout << "PASS dropdown selection/dismiss/keyboard/flip and 1:1 glyph mapping at 100/125/150/200 percent\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
