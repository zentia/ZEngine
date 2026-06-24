#include "ZSlateDemoWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"       // native input bus (P10)
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend (M5)
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/SSlider.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <cmath>
using namespace ZSlate;

namespace
{
    std::shared_ptr<STextBlock> MakeText(const char* text, float font_size, const ZSlate::UIColor& color)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text = text;
        t->FontSize = font_size;
        t->Color = color;
        t->Alignment = ZSlate::TextAnchor::MiddleLeft;
        return t;
    }

    // label | control row.
    std::shared_ptr<SHorizontalBox> MakeRow(const std::shared_ptr<SWidget>& label,
                                            const std::shared_ptr<SWidget>& control,
                                            float scale)
    {
        auto row = std::make_shared<SHorizontalBox>();
        row->AddSlot(label).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 10.0f * scale, 0.0f));
        row->AddSlot(control).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        return row;
    }
}  // namespace

ZSlateDemoWindow::ZSlateDemoWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kZSlate)
{
    m_Open = true;
    BuildUI(1.0f);
}

void ZSlateDemoWindow::BuildUI(float scale)
{
    const ZSlate::UIColor label_color(0.74f, 0.78f, 0.84f, 1.0f);

    auto title = MakeText("ZSlate", 30.0f * scale, ZSlate::UIColor(0.96f, 0.96f, 0.99f, 1.0f));
    auto subtitle = MakeText("Retained-mode UI - P4 widget library", 16.0f * scale, ZSlate::UIColor(0.62f, 0.64f, 0.72f, 1.0f));

    // CheckBox row.
    auto check = std::make_shared<SCheckBox>();
    check->BoxSize = 18.0f * scale;
    check->Checked = m_Enabled;
    check->OnCheckStateChanged = [this](bool v) { m_Enabled = v; };
    auto check_row = std::make_shared<SHorizontalBox>();
    check_row->AddSlot(check).AutoSize().SetVAlign(EVerticalAlignment::Center).SetPadding(ZSlate::FMargin(0.0f, 0.0f, 10.0f * scale, 0.0f));
    check_row->AddSlot(MakeText("SCheckBox - enable feature", 16.0f * scale, label_color))
        .Fill(1.0f)
        .SetVAlign(EVerticalAlignment::Center);

    // Slider row.
    auto slider = std::make_shared<SSlider>();
    slider->Height = 18.0f * scale;
    slider->HandleWidth = 10.0f * scale;
    slider->MinDesiredWidth = 120.0f * scale;
    slider->Value = m_Opacity;
    slider->OnValueChanged = [this](float v) { m_Opacity = v; };
    auto slider_row = MakeRow(MakeText("SSlider - opacity", 16.0f * scale, label_color), slider, scale);

    // Text box row.
    auto text_box = std::make_shared<SEditableTextBox>();
    text_box->FontSize = 16.0f * scale;
    text_box->MinWidth = 140.0f * scale;
    text_box->Padding = ZSlate::FMargin(8.0f * scale, 4.0f * scale);
    text_box->Text = m_Name;
    text_box->HintText = "type here...";
    text_box->OnTextChanged = [this](const std::string& s) { m_Name = s; };
    auto text_row = MakeRow(MakeText("SEditableTextBox - name", 16.0f * scale, label_color), text_box, scale);

    // Button.
    auto button_label = MakeText("A ZSlate Button", 17.0f * scale, ZSlate::UIColor(1.0f, 1.0f, 1.0f, 1.0f));
    button_label->Alignment = ZSlate::TextAnchor::MiddleCenter;
    auto button = std::make_shared<SButton>();
    button->Padding = ZSlate::FMargin(14.0f * scale, 8.0f * scale);
    button->SetContent(button_label);
    button->OnClicked = [this]() { ++m_ClickCount; };

    m_StatusLabel = MakeText("", 15.0f * scale, ZSlate::UIColor(0.78f, 0.72f, 0.55f, 1.0f));

    // Scrollable body: the rows above + filler rows to demonstrate SScrollBox.
    auto scroll = std::make_shared<SScrollBox>();
    auto add_scroll_row = [&](const std::shared_ptr<SWidget>& w) {
        auto wrap = std::make_shared<SBorder>();
        wrap->DrawBackground = false;
        wrap->Padding = ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale);
        wrap->HAlign = EHorizontalAlignment::Fill;
        wrap->VAlign = EVerticalAlignment::Top;
        wrap->SetContent(w);
        scroll->AddChild(wrap);
    };
    add_scroll_row(check_row);
    add_scroll_row(slider_row);
    add_scroll_row(text_row);
    add_scroll_row(button);
    add_scroll_row(m_StatusLabel);
    add_scroll_row(MakeText("--- SScrollBox demo (scroll with the wheel) ---", 14.0f * scale,
                            ZSlate::UIColor(0.50f, 0.52f, 0.58f, 1.0f)));
    for (int i = 1; i <= 14; ++i)
    {
        const std::string row_text = "Filler property #" + std::to_string(i);
        add_scroll_row(MakeText(row_text.c_str(), 15.0f * scale, ZSlate::UIColor(0.66f, 0.70f, 0.76f, 1.0f)));
    }

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(title).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 8.0f * scale));
    column->AddSlot(subtitle).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 14.0f * scale));
    column->AddSlot(scroll).Fill(1.0f);

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = ZSlate::FMargin(20.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(column);

    m_Root = border;
}

void ZSlateDemoWindow::OnGUI()
{
    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;
    if (m_Root == nullptr || std::abs(ui_scale - m_BuiltScale) > 0.01f)
    {
        BuildUI(ui_scale);
        m_BuiltScale = ui_scale;
        m_Input.Reset();
    }

    if (m_StatusLabel)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "State -> enabled:%s  opacity:%.2f  name:\"%s\"  clicks:%d",
                      m_Enabled ? "true" : "false", m_Opacity, m_Name.c_str(), m_ClickCount);
        m_StatusLabel->Text = buf;
    }

    // P10c: native-host panels source their leaf rect from EditorView::NativeRect()
    // (no ImGui::Begin / item to probe); otherwise use the ImGui content region.
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    // P9: native RHI backend paints into the shared BatchedUIRenderer (frame managed by
    // ZSlateEditorOverlay) clipped to this panel. The SlateImGuiRenderer fallback was retired.
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        renderer.PushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.PopClipRect();
    }

    // Route input AFTER paint so hit-testing uses this frame's cached geometry.
    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed
    // EditorSlateHost (the transitional r.ZSlate.NativeInput CVar was retired in
    // P10c, so the old ImGui::GetIO() fallback branches were dead and are gone).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;
    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);

    // Keyboard only flows while a ZSlate widget holds focus (avoids stealing keys).
    if (m_Input.HasKeyboardFocus())
    {
        for (unsigned int cp : host.GetCharsThisFrame())
            m_Input.ProcessChar(cp);
        // UE pattern: route ALL keys to the focused widget
        for (EKey key : host.GetKeysThisFrame())
            m_Input.ProcessKey(key);
    }
}
