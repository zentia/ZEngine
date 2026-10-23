#include "PlayModeView.h"

#include "Editor/EditorInputManager/EditorInputManager.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <cstring>

// 探针：编辑器场景视口（ViewportType::scene）在 macOS 上曾经一直是 0x0，而 axis
// gizmo 就画在这个视口上 —— 视口是 0 的话，就算 pass 有了 axis 绘制路径也什么都
// 看不见，而且不报任何错。这里把"谁把视口清成 0 / 发布了什么矩形"打出来：
// ClearViewport 是**窗口没打开（或不是激活 tab）**，UpdateViewport 里 rect 为 0 是
// **窗口开着但布局给了 0 尺寸**，两者修法完全不同。只在值变化时打，不刷屏。
namespace
{
    void LogViewportProbe(const char* where, int viewport_id, float x, float y, float w, float h)
    {
        static float s_last[2][5] = {{-1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1}};
        float        cur[5]       = {x, y, w, h, static_cast<float>(viewport_id)};
        // where 首字母：'C' = ClearViewport（恒为 0），'U' = published。
        float* last = s_last[where[0] == 'C' ? 0 : 1];
        if (last[0] == cur[0] && last[1] == cur[1] && last[2] == cur[2] && last[3] == cur[3] &&
            last[4] == cur[4])
        {
            return;
        }
        std::memcpy(last, cur, sizeof(cur));
        LOG_INFO(ZEditor,
                 "PlayModeView: {} viewport_id={} rect=({:.0f},{:.0f}) {:.0f}x{:.0f}",
                 where,
                 viewport_id,
                 x,
                 y,
                 w,
                 h);
    }
}  // namespace

PlayModeView::PlayModeView(EditorUI* editor_ui, const char* name, const ViewportType viewport_id)
    : EditorWindow(editor_ui, name), m_ViewportId(viewport_id)
{
    LOG_INFO(ZEditor, "PlayModeView: constructed '{}' viewport_id={}", name, static_cast<int>(viewport_id));
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
    LogViewportProbe("ClearViewport", static_cast<int>(m_ViewportId), 0.0f, 0.0f, 0.0f, 0.0f);
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
        const auto fb_scale = host.GetFramebufferScale();  // returns ZSlate::Vector2
        scale_x = fb_scale.x;
        scale_y = fb_scale.y;
        main_x = host.GetDisplayPos().x;
        main_y = host.GetDisplayPos().y;
    }

    if (width <= 0.0f || height <= 0.0f)
    {
        // 和 ClearViewport 区分开：这条是"窗口开着但 native rect 是 0"。
        LogViewportProbe("published/native-rect-empty",
                         static_cast<int>(m_ViewportId),
                         work_x,
                         work_y,
                         width,
                         height);
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

    LogViewportProbe("published",
                     static_cast<int>(m_ViewportId),
                     render_target_window_pos.x,
                     render_target_window_pos.y,
                     pixel_width,
                     pixel_height);
}
