#include "ZSlateGameWindow.h"

#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend

#include "Runtime/Application/Application.h"  // g_isPlaying
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <cstdio>
using namespace ZSlate;

namespace
{
    const ZSlate::UIColor kEditColor(0.0f, 1.0f, 0.0f, 1.0f);
    const ZSlate::UIColor kPlayColor(0.95f, 0.85f, 0.20f, 1.0f);

    std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text = text;
        t->FontSize = font_size;
        t->Color = color;
        t->Alignment = ZSlate::TextAnchor::MiddleLeft;
        return t;
    }
}  // namespace

ZSlateGameWindow::ZSlateGameWindow(EditorUI* editor_ui)
    : PlayModeView(editor_ui, EditorLayoutWindowIds::kGame, ViewportType::game)
{
    // NoBackground: the game render target composites under the panel.
    // MenuBar: keep the reserved menu-bar strip so the docked layout matches the
    // legacy GameWindow (and Scene/Game stay visually consistent).
    m_WindowFlags = EditorViewFlags_NoBackground | EditorViewFlags_MenuBar;
}

void ZSlateGameWindow::BuildOverlay(bool editor_mode, float camera_speed, float scale)
{
    auto column = std::make_shared<SVerticalBox>();

    if (editor_mode)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Current editor camera move speed: [%f]", camera_speed);
        column->AddSlot(MakeText(buf, 14.0f * scale, kEditColor)).AutoSize();
    }
    else
    {
        column->AddSlot(MakeText("Press Left Alt to lock or unlock the mouse cursor.", 14.0f * scale, kPlayColor))
            .AutoSize();
    }

    m_Root = column;
}

void ZSlateGameWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    const bool editor_mode = !g_isPlaying;
    const float camera_speed = editor_mode ? GET_SYSTEM(EditorInputManager)->getCameraSpeed() : 0.0f;

    const bool rebuild = (m_Root == nullptr) || (editor_mode != m_BuiltEditorMode) ||
                         (ui_scale != m_BuiltScale) ||
                         (editor_mode && camera_speed != m_BuiltCameraSpeed);
    if (rebuild)
    {
        BuildOverlay(editor_mode, camera_speed, ui_scale);
        m_BuiltEditorMode = editor_mode;
        m_BuiltCameraSpeed = camera_speed;
        m_BuiltScale = ui_scale;
    }

    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float avail_w = 0.0f;
    float avail_h = 0.0f;
    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    pos_x = native_rect[0];
    pos_y = native_rect[1];
    avail_w = native_rect[2];
    avail_h = native_rect[3];
    if (avail_w < 1.0f)
        avail_w = 1.0f;
    if (avail_h < 1.0f)
        avail_h = 1.0f;

    const ZSlate::UIRect region(pos_x, pos_y, avail_w, avail_h);
    const FGeometry geometry(ZSlate::Vector2(pos_x, pos_y), ZSlate::Vector2(avail_w, avail_h));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    BatchedUIRenderer& renderer = overlay.GetRenderer();
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

    m_Root->CacheDesiredSize();

    FPaintContext ctx;
    ctx.Renderer = &renderer;
    ctx.LayerId = 0;

    renderer.PushClipRect(region, true);
    m_Root->Paint(ctx, geometry);
    renderer.PopClipRect();

    UpdateViewport();
}
