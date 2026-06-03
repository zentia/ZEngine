#include "PlayModeView.h"

#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/WindowSystem.h"

PlayModeView::PlayModeView(EditorUI* editor_ui, const char* name, const ViewportType viewport_id)
    : EditorWindow(editor_ui, name), m_ViewportId(viewport_id)
{
}

EditorWindowState PlayModeView::BeginGUI()
{
    EditorWindowState state = EditorWindow::BeginGUI();
    if (state != EditorWindowState::Opened)
    {
        ClearViewport();
    }
    return state;
}

void PlayModeView::ClearViewport()
{
    GET_SYSTEM(RenderSystem)->UpdateViewport(m_ViewportId, 0.0f, 0.0f, 0.0f, 0.0f);
    OnViewportHidden();
}

void PlayModeView::UpdateViewport()
{
    float work_x = 0.0f;
    float work_y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float main_x = 0.0f;
    float main_y = 0.0f;

    // Native dock hosting is unconditional: the panel sources its leaf rect from
    // NativeRect() and display metrics from the GLFW-backed EditorSlateHost.
    {
        const float* rect = NativeRect();
        work_x = rect[0];
        work_y = rect[1];
        width = rect[2];
        height = rect[3];
        const ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
        const Vector2 fb_scale = host.GetFramebufferScale();
        scale_x = fb_scale.x;
        scale_y = fb_scale.y;
        main_x = host.GetDisplayPos().x;
        main_y = host.GetDisplayPos().y;
    }

    if (width <= 0.0f || height <= 0.0f)
    {
        ClearViewport();
        return;
    }

    Vector2 render_target_window_pos;
    render_target_window_pos.x = (work_x - main_x) * scale_x;
    render_target_window_pos.y = (work_y - main_y) * scale_y;

    const float pixel_width = width * scale_x;
    const float pixel_height = height * scale_y;

    GET_SYSTEM(RenderSystem)
        ->UpdateViewport(m_ViewportId, render_target_window_pos.x, render_target_window_pos.y, pixel_width, pixel_height);
}
