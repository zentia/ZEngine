#include "ZSlateScriptPanelWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/Scripting/EditorScriptHooks.h"
#include "Editor/Scripting/EditorScriptPanelModel.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <string>

using namespace ZSlate;

namespace
{
    const ZSlate::UIColor kTextColor(0.86f, 0.88f, 0.92f, 1.0f);
    const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);

    std::shared_ptr<STextBlock> MakeText(const std::string& text, float size, const ZSlate::UIColor& color)
    {
        auto block = std::make_shared<STextBlock>();
        block->Text = text;
        block->FontSize = size;
        block->Color = color;
        return block;
    }
}  // namespace

ZSlateScriptPanelWindow::ZSlateScriptPanelWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kScriptPanel)
{
}

void ZSlateScriptPanelWindow::RebuildContent(float scale)
{
    m_Root = std::make_shared<SVerticalBox>();

    const std::vector<std::string>& lines = EditorScriptPanelModel::GetLines();
    const std::vector<EditorScriptPanelModel::Button>& buttons = EditorScriptPanelModel::GetButtons();

    if (lines.empty() && buttons.empty())
    {
        // Says what to do rather than just being blank: an empty panel is
        // otherwise indistinguishable from a broken one.
        m_Root->AddSlot(MakeText("Nothing pushed yet. An editor script fills this with "
                                 "EditorScriptPanel.SetLines([...]).",
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(8.0f * scale, 8.0f * scale));
        return;
    }

    for (const std::string& line : lines)
    {
        m_Root->AddSlot(MakeText(line, 14.0f * scale, kTextColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(8.0f * scale, 2.0f * scale, 8.0f * scale, 2.0f * scale));
    }

    if (!buttons.empty())
    {
        auto row = std::make_shared<SHorizontalBox>();
        for (const EditorScriptPanelModel::Button& button : buttons)
        {
            auto widget = std::make_shared<SButton>();
            widget->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
            widget->SetContent(MakeText(button.label, 14.0f * scale, kTextColor));
            // Capture the names, not the model entry: the model can be rebuilt
            // by a re-push between laying this row out and clicking it, and
            // Invoke() re-resolves module/function by name anyway.
            const EditorScriptHooks::Callback callback = button.callback;
            widget->OnClicked = [callback]() { EditorScriptHooks::Invoke(callback); };
            row->AddSlot(widget).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 6.0f * scale, 0.0f));
        }
        m_Root->AddSlot(row).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 8.0f * scale));
    }
}

void ZSlateScriptPanelWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    const uint64_t revision = EditorScriptPanelModel::GetRevision();
    if (m_Root == nullptr || ui_scale != m_BuiltScale || revision != m_BuiltRevision)
    {
        m_BuiltScale = ui_scale;
        m_BuiltRevision = revision;
        RebuildContent(ui_scale);
        m_Input.Reset();
    }

    // ---- Paint --------------------------------------------------------------
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &overlay;
        ctx.LayerId = 0;
        renderer.PushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.PopClipRect();
    }

    // ---- Input --------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);
}
