#include "Editor/FloatingPanel/FloatingPanelManager.h"

#include "Editor/EditorLayout/DefaultLayout/DefaultLayout.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Widgets/SWindowTitleBar.h"
#include "Runtime/UI/Render/BatchedUIRenderer.h"

#include <GLFW/glfw3.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

#if defined(_WIN32)
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"

    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

namespace
{
    // Per-floating-window input accumulators. Stored as the GLFW window user pointer
    // so the C callbacks (which can only see a void*) can write into them. Kept at
    // file scope (not nested in the private FloatingPanel) precisely so the free
    // callbacks below can name the type. Drained + reset each BuildBatches.
    struct FloatingInputAccum
    {
        bool left_down {false};
        bool right_down {false};
        bool middle_down {false};
        bool ctrl {false};
        bool shift {false};
        bool alt {false};

        int pending_presses {0};
        int pending_releases {0};
        int pending_double_clicks {0};
        float pending_wheel {0.0f};
        std::vector<unsigned int> pending_chars;
        std::vector<ZSlate::EKey> pending_keys;

        double last_press_time {-1.0};
        Vector2 last_press_pos {0.0f, 0.0f};
        Vector2 prev_pointer {0.0f, 0.0f};
    };

    // Minimal GLFW->Slate key map (same subset EditorSlateHost uses; duplicated
    // because its copy lives in an unexported anonymous namespace).
    ZSlate::EKey MapGlfwKeyToSlateFloating(int glfw_key)
    {
        switch (glfw_key)
        {
            case GLFW_KEY_BACKSPACE: return ZSlate::EKey::Backspace;
            case GLFW_KEY_DELETE:    return ZSlate::EKey::Delete;
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER:  return ZSlate::EKey::Enter;
            case GLFW_KEY_ESCAPE:    return ZSlate::EKey::Escape;
            case GLFW_KEY_LEFT:      return ZSlate::EKey::Left;
            case GLFW_KEY_RIGHT:     return ZSlate::EKey::Right;
            case GLFW_KEY_HOME:      return ZSlate::EKey::Home;
            case GLFW_KEY_END:       return ZSlate::EKey::End;
            case GLFW_KEY_SPACE:     return ZSlate::EKey::Space;
            default:                 return ZSlate::EKey::Unknown;
        }
    }

    void FloatingMouseButtonCb(GLFWwindow* window, int button, int action, int mods)
    {
        auto* in = static_cast<FloatingInputAccum*>(glfwGetWindowUserPointer(window));
        if (in == nullptr)
            return;
        const bool down = (action != GLFW_RELEASE);
        if (button == GLFW_MOUSE_BUTTON_LEFT)
        {
            in->left_down = down;
            if (action == GLFW_PRESS)
            {
                ++in->pending_presses;
                constexpr double kDoubleClickTime = 0.30;
                constexpr float kDoubleClickMaxDist = 6.0f;
                double cx = 0.0, cy = 0.0;
                glfwGetCursorPos(window, &cx, &cy);
                const double now = glfwGetTime();
                const float ddx = static_cast<float>(cx) - in->last_press_pos.x;
                const float ddy = static_cast<float>(cy) - in->last_press_pos.y;
                if (in->last_press_time >= 0.0 && (now - in->last_press_time) <= kDoubleClickTime &&
                    (ddx * ddx + ddy * ddy) <= (kDoubleClickMaxDist * kDoubleClickMaxDist))
                {
                    ++in->pending_double_clicks;
                    in->last_press_time = -1.0;
                }
                else
                {
                    in->last_press_time = now;
                }
                in->last_press_pos = Vector2(static_cast<float>(cx), static_cast<float>(cy));
            }
            else if (action == GLFW_RELEASE)
            {
                ++in->pending_releases;
            }
        }
        else if (button == GLFW_MOUSE_BUTTON_RIGHT)
        {
            in->right_down = down;
        }
        else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
        {
            in->middle_down = down;
        }
        in->ctrl = (mods & GLFW_MOD_CONTROL) != 0;
        in->shift = (mods & GLFW_MOD_SHIFT) != 0;
        in->alt = (mods & GLFW_MOD_ALT) != 0;
    }

    void FloatingScrollCb(GLFWwindow* window, double /*xoff*/, double yoff)
    {
        if (auto* in = static_cast<FloatingInputAccum*>(glfwGetWindowUserPointer(window)))
            in->pending_wheel += static_cast<float>(yoff);
    }

    void FloatingKeyCb(GLFWwindow* window, int key, int /*scan*/, int action, int mods)
    {
        auto* in = static_cast<FloatingInputAccum*>(glfwGetWindowUserPointer(window));
        if (in == nullptr)
            return;
        in->ctrl = (mods & GLFW_MOD_CONTROL) != 0;
        in->shift = (mods & GLFW_MOD_SHIFT) != 0;
        in->alt = (mods & GLFW_MOD_ALT) != 0;
        if (action == GLFW_PRESS || action == GLFW_REPEAT)
        {
            const ZSlate::EKey k = MapGlfwKeyToSlateFloating(key);
            if (k != ZSlate::EKey::Unknown)
                in->pending_keys.push_back(k);
        }
    }

    void FloatingCharCb(GLFWwindow* window, unsigned int codepoint)
    {
        if (auto* in = static_cast<FloatingInputAccum*>(glfwGetWindowUserPointer(window)))
            in->pending_chars.push_back(codepoint);
    }
}  // namespace

// FloatingPanel: one detached panel + its OS window + GPU surface + paint batch +
// per-window input accumulators (P5).
struct FloatingPanelManager::FloatingPanel
{
    std::string title;
    GLFWwindow* window {nullptr};
    BatchedUIRenderer renderer;
    int width {0};
    int height {0};
    bool batch_ready {false};
    FloatingInputAccum input;
    int win_x {0};
    int win_y {0};
    int frames_alive {0};

    // UE-style window chrome as real ZSlate widgets, routed by chrome_input in the
    // panel's own pixel space (built lazily in EnsureChrome). The widgets fire
    // intent delegates; the manager turns them into GLFW window ops.
    std::shared_ptr<ZSlate::SWindowFrame> chrome;
    ZSlate::SlateInputRouter chrome_input;
    Vector2 caption_grab {0.0f, 0.0f};  // client-local PIXEL grab point at drag start
    bool caption_dragging {false};
    bool grip_resizing {false};
    // Lets the router observe a full down->up for a click that pressed AND released
    // inside one frame (between polls). Feeds a synthetic "up" the next frame.
    bool router_owes_up {false};

    // Maximize / restore (borderless windows have no OS maximize button, so we drive
    // it: maximize covers the monitor work area; restore returns to the saved rect).
    bool maximized {false};
    int restore_x {0};
    int restore_y {0};
    int restore_w {0};
    int restore_h {0};
#if defined(_WIN32)
    DX12FloatingSurface* surface {nullptr};
#endif
};

namespace
{
    // Custom title bar / resize-grip metrics, in LOGICAL (screen) coords. Drawing
    // multiplies by ui_scale to land in the floating surface's pixel space; hit-tests
    // run in logical coords (glfwGetCursorPos / glfwGetWindowSize are logical).
    constexpr float kTitleBarLogicalH = 26.0f;
    constexpr float kResizeGripLogical = 16.0f;
    constexpr int kMinFloatW = 240;
    constexpr int kMinFloatH = 160;

    // Map a widget's requested cursor (Runtime ECursorType) onto the editor host's
    // platform cursor (UE FCursorReply -> FSlateApplication::SetCursor analogue).
    ZSlate::EMouseCursor MapSlateCursor(ZSlate::ECursorType c)
    {
        switch (c)
        {
            case ZSlate::ECursorType::Hand:       return ZSlate::EMouseCursor::Hand;
            case ZSlate::ECursorType::ResizeEW:   return ZSlate::EMouseCursor::ResizeEW;
            case ZSlate::ECursorType::ResizeNS:   return ZSlate::EMouseCursor::ResizeNS;
            case ZSlate::ECursorType::ResizeNWSE: return ZSlate::EMouseCursor::ResizeNWSE;
            default:                              return ZSlate::EMouseCursor::Default;
        }
    }

    // Framebuffer-pixel / window-coord ratio. On Windows window coords ARE pixels
    // (ratio 1, even at 150% DPI); on macOS window coords are points (ratio 2).
    // GLFW window position / size ops are in WINDOW coords, while our chrome widgets
    // live in PIXEL space, so this is the one conversion at the OS boundary.
    void FramebufferRatio(GLFWwindow* window, float& fbx, float& fby)
    {
        fbx = 1.0f;
        fby = 1.0f;
        if (window == nullptr)
            return;
        int fbw = 0;
        int fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        int win_w = 0;
        int win_h = 0;
        glfwGetWindowSize(window, &win_w, &win_h);
        if (win_w > 0)
            fbx = static_cast<float>(fbw) / static_cast<float>(win_w);
        if (win_h > 0)
            fby = static_cast<float>(fbh) / static_cast<float>(win_h);
    }

    // Work area (excludes the taskbar) of the monitor that contains the window centre;
    // falls back to the primary monitor. Used for borderless maximize.
    void ComputeWorkAreaForWindow(GLFWwindow* window, int& out_x, int& out_y, int& out_w, int& out_h)
    {
        int wx = 0;
        int wy = 0;
        int ww = 0;
        int wh = 0;
        glfwGetWindowPos(window, &wx, &wy);
        glfwGetWindowSize(window, &ww, &wh);
        const int cx = wx + ww / 2;
        const int cy = wy + wh / 2;

        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        GLFWmonitor* best = glfwGetPrimaryMonitor();
        for (int i = 0; i < count; ++i)
        {
            int mx = 0;
            int my = 0;
            int mw = 0;
            int mh = 0;
            glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
            if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh)
            {
                best = monitors[i];
                break;
            }
        }
        if (best != nullptr)
        {
            glfwGetMonitorWorkarea(best, &out_x, &out_y, &out_w, &out_h);
        }
        else
        {
            out_x = wx;
            out_y = wy;
            out_w = ww;
            out_h = wh;
        }
    }
}  // namespace

FloatingPanelManager::FloatingPanelManager() = default;
FloatingPanelManager::~FloatingPanelManager() = default;

FloatingPanelManager& FloatingPanelManager::Get()
{
    static FloatingPanelManager instance;
    return instance;
}

void FloatingPanelManager::Initialize(EditorUI* editor_ui)
{
    m_EditorUi = editor_ui;
}

DefaultLayout* FloatingPanelManager::ResolveLayout() const
{
    return m_EditorUi != nullptr ? m_EditorUi->getLayoutManager() : nullptr;
}

void FloatingPanelManager::RequestFloat(const std::string& title)
{
#if !defined(_WIN32)
    LOG_WARNING(ZEditor, "FloatingPanelManager: tear-off is Windows/DX12 only (ignored '%s')", title.c_str());
    return;
#else
    if (title.empty() || IsFloating(title))
        return;
    for (const PendingFloat& p : m_PendingFloat)
    {
        if (p.title == title)
            return;
    }
    PendingFloat req;
    req.title = title;
    m_PendingFloat.push_back(std::move(req));
#endif
}

void FloatingPanelManager::RequestFloatAt(const std::string& title, int x, int y, int width, int height)
{
#if !defined(_WIN32)
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    LOG_WARNING(ZEditor, "FloatingPanelManager: tear-off is Windows/DX12 only (ignored '%s')", title.c_str());
    return;
#else
    if (title.empty() || IsFloating(title))
        return;
    for (PendingFloat& p : m_PendingFloat)
    {
        if (p.title == title)
        {
            p.has_rect = true;
            p.x = x;
            p.y = y;
            p.width = width;
            p.height = height;
            return;
        }
    }
    PendingFloat req;
    req.title = title;
    req.has_rect = true;
    req.x = x;
    req.y = y;
    req.width = width;
    req.height = height;
    m_PendingFloat.push_back(std::move(req));
#endif
}

void FloatingPanelManager::RequestDock(const std::string& title)
{
    if (title.empty())
        return;
    if (std::find(m_PendingDock.begin(), m_PendingDock.end(), title) == m_PendingDock.end())
        m_PendingDock.push_back(title);
}

bool FloatingPanelManager::IsFloating(const std::string& title) const
{
    for (const auto& panel : m_Panels)
    {
        if (panel->title == title)
            return true;
    }
    return false;
}

bool FloatingPanelManager::HasFloatingPanels() const
{
    return !m_Panels.empty();
}

void FloatingPanelManager::ProcessPendingFloat()
{
#if defined(_WIN32)
    if (m_PendingFloat.empty())
        return;

    DefaultLayout* layout = ResolveLayout();
    WindowSystem* window_system = GET_SYSTEM(WindowSystem).get();
    std::shared_ptr<RHI> rhi_base = GET_SYSTEM(RenderSystem) ? GET_SYSTEM(RenderSystem)->GetRHI() : nullptr;
    if (layout == nullptr || window_system == nullptr || rhi_base == nullptr)
    {
        // Not ready yet (e.g. RHI still booting on the first ticks after a session
        // restore). Keep the queue and retry next frame rather than dropping it.
        return;
    }
    DX12RHI* dx12 = (rhi_base->getGraphicsAPI() == GraphicsAPI::DirectX12)
                        ? static_cast<DX12RHI*>(rhi_base.get())
                        : nullptr;
    if (dx12 == nullptr)
    {
        LOG_WARNING(ZEditor, "FloatingPanelManager: active RHI is not DX12; tear-off unavailable");
        m_PendingFloat.clear();
        return;
    }

    std::vector<PendingFloat> pending;
    pending.swap(m_PendingFloat);
    for (const PendingFloat& req : pending)
    {
        const std::string& title = req.title;
        if (IsFloating(title))
            continue;
        EditorWindow* editor_window = m_EditorUi->FindEditorWindow(title);
        if (editor_window == nullptr)
        {
            LOG_WARNING(ZEditor, "FloatingPanelManager: no editor window titled '%s'", title.c_str());
            continue;
        }

        int pos_x = 120;
        int pos_y = 120;
        int win_w = 900;
        int win_h = 600;
        if (req.has_rect && req.width > 1 && req.height > 1)
        {
            // Explicit spawn (tab-drag tear-off or session restore).
            pos_x = req.x;
            pos_y = req.y;
            win_w = std::max(req.width, 240);
            win_h = std::max(req.height, 160);
        }
        else
        {
            // Seed from the panel's current dock rect (absolute desktop coords) so
            // it pops out roughly where it was docked.
            float rect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            bool is_active = false;
            if (layout->QueryNativeDockPanel(title.c_str(), rect, is_active) && rect[2] > 1.0f && rect[3] > 1.0f)
            {
                pos_x = static_cast<int>(rect[0]);
                pos_y = static_cast<int>(rect[1]);
                win_w = std::max(static_cast<int>(rect[2]), 240);
                win_h = std::max(static_cast<int>(rect[3]), 160);
            }
        }

        // Borderless: we draw our own title bar so the tab drag is handled in-app
        // (window follows the cursor live, re-docks on drop -- UE custom-chrome model).
        GLFWwindow* child = window_system->CreateChildWindow(title.c_str(), win_w, win_h, pos_x, pos_y, /*decorated=*/false);
        if (child == nullptr)
        {
            LOG_WARNING(ZEditor, "FloatingPanelManager: CreateChildWindow failed for '%s'", title.c_str());
            continue;
        }

        int fb_w = win_w;
        int fb_h = win_h;
        glfwGetFramebufferSize(child, &fb_w, &fb_h);
        fb_w = std::max(fb_w, 1);
        fb_h = std::max(fb_h, 1);

        HWND hwnd = glfwGetWin32Window(child);
        DX12FloatingSurface* surface =
            dx12->CreateFloatingSurface(reinterpret_cast<void*>(hwnd),
                                        static_cast<uint32_t>(fb_w),
                                        static_cast<uint32_t>(fb_h));
        if (surface == nullptr)
        {
            LOG_WARNING(ZEditor, "FloatingPanelManager: CreateFloatingSurface failed for '%s'", title.c_str());
            window_system->DestroyChildWindow(child);
            continue;
        }

        // Pull the panel out of the dock tree (its docked siblings reflow).
        layout->SetPanelFloating(title.c_str(), true);

        auto panel = std::make_unique<FloatingPanel>();
        panel->title = title;
        panel->window = child;
        panel->surface = surface;
        panel->width = fb_w;
        panel->height = fb_h;
        panel->win_x = pos_x;
        panel->win_y = pos_y;
        FloatingPanel* panel_raw = panel.get();
        m_Panels.push_back(std::move(panel));

        // P5 input: route this window's GLFW events into the panel's accumulators.
        // The user pointer is the accumulator address (stable inside the unique_ptr).
        glfwSetWindowUserPointer(child, &panel_raw->input);
        glfwSetMouseButtonCallback(child, FloatingMouseButtonCb);
        glfwSetScrollCallback(child, FloatingScrollCb);
        glfwSetKeyCallback(child, FloatingKeyCb);
        glfwSetCharCallback(child, FloatingCharCb);

        glfwShowWindow(child);
        LOG_INFO(ZEditor, "FloatingPanelManager: floated panel '%s' (%dx%d)", title.c_str(), fb_w, fb_h);
    }
    if (!pending.empty())
        SaveState();
#endif
}

void FloatingPanelManager::DestroyPanel(FloatingPanel& panel, bool redock)
{
    auto render_system = GET_SYSTEM(RenderSystem);
    std::shared_ptr<RHI> rhi_base = render_system ? render_system->GetRHI() : nullptr;

    // Free the per-window GPU ring in the overlay.
#if defined(_WIN32)
    if (rhi_base != nullptr && panel.surface != nullptr)
    {
        // CRITICAL: with parallel rendering the render/RHI thread may still be mid-frame
        // and referencing this floating surface. DestroyFloatingSurface does a fenced
        // WaitForGpuIdle + releases the swapchain, and we then destroy the GLFW window.
        // Doing that while the render thread holds the queue deadlocks (INFINITE fence
        // wait). Drain the pipeline first -- same handshake the live-resize path uses --
        // so the surface is no longer referenced before we tear it down.
        if (render_system != nullptr)
            render_system->FlushRenderingCommands();

        ZSlate::ZSlateEditorOverlay::Get().ReleaseFloatingRing(rhi_base, panel.surface);

        if (rhi_base->getGraphicsAPI() == GraphicsAPI::DirectX12)
        {
            static_cast<DX12RHI*>(rhi_base.get())->DestroyFloatingSurface(panel.surface);
            panel.surface = nullptr;
        }
    }
#endif

    if (panel.window != nullptr)
    {
        if (WindowSystem* window_system = GET_SYSTEM(WindowSystem).get())
            window_system->DestroyChildWindow(panel.window);
        panel.window = nullptr;
    }

    if (redock)
    {
        if (DefaultLayout* layout = ResolveLayout())
            layout->SetPanelFloating(panel.title.c_str(), false);  // reconcile re-docks it
    }
}

void FloatingPanelManager::ProcessClosedWindows()
{
    bool removed = false;

    // Explicit re-dock requests first.
    if (!m_PendingDock.empty())
    {
        std::vector<std::string> pending;
        pending.swap(m_PendingDock);
        for (const std::string& title : pending)
        {
            for (auto it = m_Panels.begin(); it != m_Panels.end(); ++it)
            {
                if ((*it)->title == title)
                {
                    DestroyPanel(**it, /*redock=*/true);
                    m_Panels.erase(it);
                    removed = true;
                    break;
                }
            }
        }
    }

    // OS-driven close (user clicked the window's X) -> re-dock.
    for (auto it = m_Panels.begin(); it != m_Panels.end();)
    {
        FloatingPanel& panel = **it;
        if (panel.window != nullptr && glfwWindowShouldClose(panel.window))
        {
            LOG_INFO(ZEditor, "FloatingPanelManager: re-docking '%s' (window closed)", panel.title.c_str());
            DestroyPanel(panel, /*redock=*/true);
            it = m_Panels.erase(it);
            removed = true;
        }
        else
        {
            ++it;
        }
    }

    if (removed)
        SaveState();
}

void FloatingPanelManager::EnsureChrome(FloatingPanel& panel)
{
    if (panel.chrome)
        return;

    panel.chrome = std::make_shared<ZSlate::SWindowFrame>();
    FloatingPanel* p = &panel;

    ZSlate::SWindowTitleBar& tb = *panel.chrome->TitleBar;
    tb.Title = panel.title;
    tb.IsMaximizedQuery = [p] { return p->maximized; };
    tb.OnToggleMaximize = [this, p] { ToggleMaximize(*p); };
    tb.OnClose = [this, p] {
        LOG_INFO(ZEditor, "FloatingPanelManager: re-docking '%s' (close button)", p->title.c_str());
        RequestDock(p->title);
    };
    tb.OnCaptionDragBegin = [this, p](const Vector2& pos) { BeginCaptionDrag(*p, pos); };
    tb.OnCaptionDrag = [this, p](const Vector2& pos) { UpdateCaptionDrag(*p, pos); };
    tb.OnCaptionDragEnd = [this, p](const Vector2& pos) { EndCaptionDrag(*p, pos); };

    ZSlate::SResizeGrip& grip = *panel.chrome->Grip;
    grip.OnResizeBegin = [p](const Vector2&) { p->grip_resizing = true; };
    grip.OnResize = [this, p](const Vector2& pos) { UpdateResize(*p, pos); };
    grip.OnResizeEnd = [p] { p->grip_resizing = false; };
}

void FloatingPanelManager::BeginCaptionDrag(FloatingPanel& panel, const Vector2& pos_px)
{
#if defined(_WIN32)
    if (panel.window == nullptr)
        return;
    if (panel.maximized)
    {
        // UE: dragging a maximized window restores it, then it follows the cursor.
        // Re-anchor the grab so the cursor stays over the (now smaller) title bar.
        RestoreFromMaximize(panel);
        float fbx = 1.0f;
        float fby = 1.0f;
        FramebufferRatio(panel.window, fbx, fby);
        int lw = 0;
        int lh = 0;
        glfwGetWindowSize(panel.window, &lw, &lh);
        const float restored_w_px = static_cast<float>(lw) * fbx;
        const float bar_px = kTitleBarLogicalH * std::max(1.0f, fby);
        panel.caption_grab = Vector2(std::min(pos_px.x, std::max(0.0f, restored_w_px * 0.5f)),
                                     std::min(pos_px.y, bar_px * 0.5f));
    }
    else
    {
        panel.caption_grab = pos_px;
    }
    panel.caption_dragging = true;
#else
    (void)panel;
    (void)pos_px;
#endif
}

void FloatingPanelManager::UpdateCaptionDrag(FloatingPanel& panel, const Vector2& pos_px)
{
#if defined(_WIN32)
    if (!panel.caption_dragging || panel.window == nullptr)
        return;

    // Move the window so the grabbed point stays under the cursor. The grab is
    // fixed at press; once the window catches up, the cursor's client-local pos
    // returns to the grab, so the delta self-cancels -- the window tracks the hand.
    // Windows auto-captures the mouse while the button is held, so the cursor keeps
    // reporting even past the window bounds (lets us drag over the main window).
    float fbx = 1.0f;
    float fby = 1.0f;
    FramebufferRatio(panel.window, fbx, fby);
    int wx = 0;
    int wy = 0;
    glfwGetWindowPos(panel.window, &wx, &wy);
    const int dx = static_cast<int>((pos_px.x - panel.caption_grab.x) / std::max(0.01f, fbx));
    const int dy = static_cast<int>((pos_px.y - panel.caption_grab.y) / std::max(0.01f, fby));
    if (dx != 0 || dy != 0)
        glfwSetWindowPos(panel.window, wx + dx, wy + dy);
    panel.win_x = wx + dx;
    panel.win_y = wy + dy;

    // Re-dock hint: highlight the host while the cursor is over the main window.
    const Vector2 global(static_cast<float>(panel.win_x) + pos_px.x / std::max(0.01f, fbx),
                         static_cast<float>(panel.win_y) + pos_px.y / std::max(0.01f, fby));
    const bool over_main = global.x >= m_MainRect[0] && global.x <= m_MainRect[0] + m_MainRect[2] &&
                           global.y >= m_MainRect[1] && global.y <= m_MainRect[1] + m_MainRect[3];
    if (DefaultLayout* layout = ResolveLayout())
        layout->SetExternalDockHint(over_main);
#else
    (void)panel;
    (void)pos_px;
#endif
}

void FloatingPanelManager::EndCaptionDrag(FloatingPanel& panel, const Vector2& pos_px)
{
#if defined(_WIN32)
    panel.caption_dragging = false;
    if (DefaultLayout* layout = ResolveLayout())
        layout->SetExternalDockHint(false);
    if (panel.window == nullptr)
        return;

    float fbx = 1.0f;
    float fby = 1.0f;
    FramebufferRatio(panel.window, fbx, fby);
    const Vector2 global(static_cast<float>(panel.win_x) + pos_px.x / std::max(0.01f, fbx),
                         static_cast<float>(panel.win_y) + pos_px.y / std::max(0.01f, fby));
    const bool over_main = global.x >= m_MainRect[0] && global.x <= m_MainRect[0] + m_MainRect[2] &&
                           global.y >= m_MainRect[1] && global.y <= m_MainRect[1] + m_MainRect[3];
    if (over_main)
    {
        LOG_INFO(ZEditor, "FloatingPanelManager: re-docking '%s' (dropped over main window)", panel.title.c_str());
        RequestDock(panel.title);
    }
#else
    (void)panel;
    (void)pos_px;
#endif
}

void FloatingPanelManager::UpdateResize(FloatingPanel& panel, const Vector2& pos_px)
{
#if defined(_WIN32)
    if (panel.window == nullptr || panel.maximized)
        return;
    // pos_px is the cursor in client-local PIXELS = the desired bottom-right corner.
    // Convert to window coords for glfwSetWindowSize. The DXGI swapchain follows on
    // the next TickMainThread (which flushes the pipeline before ResizeBuffers).
    float fbx = 1.0f;
    float fby = 1.0f;
    FramebufferRatio(panel.window, fbx, fby);
    const int new_w = std::max(kMinFloatW, static_cast<int>(pos_px.x / std::max(0.01f, fbx)));
    const int new_h = std::max(kMinFloatH, static_cast<int>(pos_px.y / std::max(0.01f, fby)));
    glfwSetWindowSize(panel.window, new_w, new_h);
#else
    (void)panel;
    (void)pos_px;
#endif
}

void FloatingPanelManager::ToggleMaximize(FloatingPanel& panel)
{
#if defined(_WIN32)
    if (panel.window == nullptr)
        return;
    if (panel.maximized)
    {
        RestoreFromMaximize(panel);
        return;
    }
    // Save the current rect, then cover the monitor work area.
    glfwGetWindowPos(panel.window, &panel.restore_x, &panel.restore_y);
    glfwGetWindowSize(panel.window, &panel.restore_w, &panel.restore_h);
    int ax = 0;
    int ay = 0;
    int aw = 0;
    int ah = 0;
    ComputeWorkAreaForWindow(panel.window, ax, ay, aw, ah);
    glfwSetWindowPos(panel.window, ax, ay);
    glfwSetWindowSize(panel.window, std::max(aw, kMinFloatW), std::max(ah, kMinFloatH));
    panel.maximized = true;
    LOG_INFO(ZEditor, "FloatingPanelManager: maximized '%s'", panel.title.c_str());
#else
    (void)panel;
#endif
}

void FloatingPanelManager::RestoreFromMaximize(FloatingPanel& panel)
{
#if defined(_WIN32)
    if (panel.window == nullptr || !panel.maximized)
        return;
    const int w = std::max(panel.restore_w, kMinFloatW);
    const int h = std::max(panel.restore_h, kMinFloatH);
    glfwSetWindowPos(panel.window, panel.restore_x, panel.restore_y);
    glfwSetWindowSize(panel.window, w, h);
    panel.maximized = false;
    LOG_INFO(ZEditor, "FloatingPanelManager: restored '%s'", panel.title.c_str());
#else
    (void)panel;
#endif
}

void FloatingPanelManager::TickMainThread()
{
    if (!m_StateLoaded)
    {
        m_StateLoaded = true;
        LoadState();
    }

    ProcessClosedWindows();
    ProcessPendingFloat();
    // Window chrome (title-bar drag / resize / maximize / close) is now handled by
    // the SWindowFrame widget tree inside BuildBatches, on this same (main) thread.

#if defined(_WIN32)
    // Track child-window resizes -> resize the floating swapchain.
    auto render_system = GET_SYSTEM(RenderSystem);
    std::shared_ptr<RHI> rhi_base = render_system ? render_system->GetRHI() : nullptr;
    DX12RHI* dx12 = (rhi_base != nullptr && rhi_base->getGraphicsAPI() == GraphicsAPI::DirectX12)
                        ? static_cast<DX12RHI*>(rhi_base.get())
                        : nullptr;
    if (dx12 != nullptr)
    {
        // First pass: detect whether ANY surface needs a resize this frame.
        bool needs_resize = false;
        for (auto& panel_ptr : m_Panels)
        {
            FloatingPanel& panel = *panel_ptr;
            if (panel.window == nullptr || panel.surface == nullptr)
                continue;
            int fb_w = panel.width;
            int fb_h = panel.height;
            glfwGetFramebufferSize(panel.window, &fb_w, &fb_h);
            if (std::max(fb_w, 1) != panel.width || std::max(fb_h, 1) != panel.height)
            {
                needs_resize = true;
                break;
            }
        }

        // ResizeFloatingSurface does WaitForGpuIdle + DXGI ResizeBuffers, which require
        // that no in-flight render references the old backbuffers. With parallel
        // rendering the render thread may still be mid-frame, so drain the pipeline
        // once before touching any swapchain (same handshake as DestroyPanel / the
        // live window-resize path). Without this the resized surface renders garbage.
        if (needs_resize && render_system != nullptr)
            render_system->FlushRenderingCommands();

        if (needs_resize)
        {
            for (auto& panel_ptr : m_Panels)
            {
                FloatingPanel& panel = *panel_ptr;
                if (panel.window == nullptr || panel.surface == nullptr)
                    continue;
                int fb_w = panel.width;
                int fb_h = panel.height;
                glfwGetFramebufferSize(panel.window, &fb_w, &fb_h);
                fb_w = std::max(fb_w, 1);
                fb_h = std::max(fb_h, 1);
                if (fb_w != panel.width || fb_h != panel.height)
                {
                    dx12->ResizeFloatingSurface(panel.surface, static_cast<uint32_t>(fb_w),
                                                static_cast<uint32_t>(fb_h));
                    panel.width = fb_w;
                    panel.height = fb_h;
                }
            }
        }
    }
#endif
}

void FloatingPanelManager::BuildBatches()
{
    if (m_Panels.empty() || m_EditorUi == nullptr)
        return;
    DefaultLayout* layout = ResolveLayout();
    if (layout == nullptr)
        return;

    ZSlate::ZSlateEditorOverlay& overlay = ZSlate::ZSlateEditorOverlay::Get();
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();

    // Capture the MAIN window rect now, BEFORE any per-panel input override is
    // pushed below -- the override makes GetDisplayPos/Size report the floating
    // window instead of the main one. The caption-drag re-dock test reads m_MainRect.
    const Vector2 main_pos = host.GetDisplayPos();
    const Vector2 main_size = host.GetDisplaySize();
    m_MainRect[0] = main_pos.x;
    m_MainRect[1] = main_pos.y;
    m_MainRect[2] = main_size.x;
    m_MainRect[3] = main_size.y;
    bool any_dragging = false;

    for (auto& panel_ptr : m_Panels)
    {
        FloatingPanel& panel = *panel_ptr;
        panel.batch_ready = false;
        EditorWindow* editor_window = m_EditorUi->FindEditorWindow(panel.title);
        if (editor_window == nullptr || panel.width <= 0 || panel.height <= 0)
            continue;

        // P5: snapshot this window's input for this frame (client-local coords; the
        // panel is placed at origin 0,0). Cursor position is polled live; buttons /
        // wheel / keys / chars come from the GLFW-callback accumulators.
        FloatingInputAccum& in = panel.input;
        ZSlate::FloatingInputState state_in;
        state_in.window = panel.window;
        float scale = 1.0f;
        if (panel.window != nullptr)
        {
            float xs = 1.0f, ys = 1.0f;
            glfwGetWindowContentScale(panel.window, &xs, &ys);
            scale = std::max(1.0f, std::max(xs, ys));

            // Cursor -> surface (pixel) space. This panel is laid out in FRAMEBUFFER PIXEL
            // space (display_w/h = panel.width/height from glfwGetFramebufferSize), so the
            // pointer must be converted by the REAL framebuffer/window ratio -- NOT the DPI
            // content scale. On Windows GLFW window coords already ARE pixels (ratio 1)
            // while content scale is e.g. 1.5; multiplying by content scale would push the
            // pointer 1.5x too far. On macOS the ratio is 2 (window coords are points), so
            // the ratio correctly doubles the pointer. ui_scale stays the content scale --
            // it sizes fonts/widgets, which is a separate concern from pointer position.
            int fbw = 0, fbh = 0;
            glfwGetFramebufferSize(panel.window, &fbw, &fbh);
            int win_w = 0, win_h = 0;
            glfwGetWindowSize(panel.window, &win_w, &win_h);
            const float fbx = win_w > 0 ? static_cast<float>(fbw) / static_cast<float>(win_w) : 1.0f;
            const float fby = win_h > 0 ? static_cast<float>(fbh) / static_cast<float>(win_h) : 1.0f;

            double cx = 0.0, cy = 0.0;
            glfwGetCursorPos(panel.window, &cx, &cy);
            state_in.pointer = Vector2(static_cast<float>(cx) * fbx, static_cast<float>(cy) * fby);
        }
        state_in.ui_scale = scale;
        state_in.pointer_delta = Vector2(state_in.pointer.x - in.prev_pointer.x, state_in.pointer.y - in.prev_pointer.y);
        in.prev_pointer = state_in.pointer;
        state_in.left_down = in.left_down;
        state_in.right_down = in.right_down;
        state_in.middle_down = in.middle_down;
        state_in.ctrl = in.ctrl;
        state_in.shift = in.shift;
        state_in.alt = in.alt;
        state_in.left_pressed = (in.pending_presses > 0);
        state_in.left_released = (in.pending_releases > 0);
        state_in.left_double = (in.pending_double_clicks > 0);
        state_in.wheel = in.pending_wheel;
        state_in.chars = std::move(in.pending_chars);
        state_in.keys = std::move(in.pending_keys);
        state_in.display_w = static_cast<float>(panel.width);
        state_in.display_h = static_cast<float>(panel.height);
        state_in.display_x = 0.0f;
        state_in.display_y = 0.0f;
        // Reset accumulators for the next frame's events.
        in.pending_presses = 0;
        in.pending_releases = 0;
        in.pending_double_clicks = 0;
        in.pending_wheel = 0.0f;
        in.pending_chars.clear();
        in.pending_keys.clear();

        // Redirect painting into this window's batch, place the panel at its
        // client-local origin, and route input to this window (the override also
        // gives the panel its own surface hit-test stack, so the main window's
        // hover is untouched).
        overlay.PushRenderer(&panel.renderer);
        host.PushInputOverride(&state_in);

        // Re-baseline this window's cursor to the default arrow every frame (the override
        // routes SetMouseCursor to panel.window). Without this, a Hand/resize cursor set
        // while hovering a panel widget would stick when the pointer moves onto the title
        // bar or empty space, because GLFW cursors are sticky per-window. The content paint
        // below, then the chrome route, re-set the cursor over their own widgets.
        host.SetMouseCursor(ZSlate::EMouseCursor::Default);

        panel.renderer.beginFrame();

        // Panel content is inset below the title bar (title_h is in surface PIXELS,
        // matching the chrome widgets which are painted + hit-tested in pixel space).
        const float title_h = kTitleBarLogicalH * state_in.ui_scale;
        const float content_y = title_h;
        const float content_h = std::max(1.0f, static_cast<float>(panel.height) - title_h);
        layout->BeginFloatingPanelRender(panel.title.c_str(), 0.0f, content_y,
                                         static_cast<float>(panel.width), content_h);

        EditorWindowState state = editor_window->BeginGUI();
        if (state == EditorWindowState::Opened)
            editor_window->OnGUI();
        editor_window->EndGUI();

        layout->EndFloatingPanelRender();

        // ---- Window chrome (UE SWindowTitleBar-style widgets) -----------------
        // Painted AFTER the content so it composites on top, then routed through one
        // SlateInputRouter in the SAME pixel space it was drawn in. Capture-based
        // SButton clicks make Maximize/Close fire only on press+release over the
        // button -- no event-accumulator edges, no press-time cursor snapshots, no
        // ui_scale / framebuffer-ratio juggling in the hit-test.
        EnsureChrome(panel);
        ZSlate::SWindowFrame& frame = *panel.chrome;
        const float bar_px = kTitleBarLogicalH * state_in.ui_scale;
        const float grip_px = kResizeGripLogical * state_in.ui_scale;
        frame.BarHeight = bar_px;
        frame.GripSize = grip_px;
        frame.Grip->Enabled = !panel.maximized;
        frame.TitleBar->Title = panel.title;
        frame.TitleBar->BarHeight = bar_px;
        frame.TitleBar->FontSize = 14.0f * state_in.ui_scale;
        frame.TitleBar->Now = ZSlate::EditorSlateHost::GetTime();

        std::shared_ptr<ZSlate::SWidget> chrome_root = panel.chrome;
        const ZSlate::FGeometry frame_geom(Vector2(0.0f, 0.0f),
                                           Vector2(static_cast<float>(panel.width),
                                                   static_cast<float>(panel.height)));
        chrome_root->CacheDesiredSize();
        ZSlate::FPaintContext paint_ctx;
        paint_ctx.Renderer = &panel.renderer;
        chrome_root->Paint(paint_ctx, frame_geom);

        // Feed the router a synthetic level so a press+release within one frame
        // (between polls) is still observed as a full down->up -> click.
        bool router_down;
        if (panel.router_owes_up)
        {
            router_down = false;
            panel.router_owes_up = false;
        }
        else if (state_in.left_pressed && !state_in.left_down)
        {
            router_down = true;
            panel.router_owes_up = true;
        }
        else
        {
            router_down = state_in.left_down;
        }

        const bool over_window = state_in.pointer.x >= 0.0f && state_in.pointer.y >= 0.0f &&
                                 state_in.pointer.x <= static_cast<float>(panel.width) &&
                                 state_in.pointer.y <= static_cast<float>(panel.height);
        panel.chrome_input.ProcessMouse(chrome_root, state_in.pointer, over_window, router_down,
                                        state_in.wheel, state_in.right_down);

        // Per-widget cursor (UE FCursorReply): only override the content's cursor
        // when the pointer is over a real chrome element, not the empty frame body.
        if (ZSlate::SWidget* hovered = panel.chrome_input.GetHoveredWidget())
        {
            if (hovered != chrome_root.get())
                host.SetMouseCursor(MapSlateCursor(hovered->GetCursor()));
        }
        any_dragging = any_dragging || panel.caption_dragging;

        host.PopInputOverride();
        overlay.PopRenderer();
        panel.batch_ready = (state == EditorWindowState::Opened);
    }

    // Reconcile the dock hint when nobody is dragging (idempotent; covers an
    // abnormal drag end so the host highlight never gets stuck on).
    if (!any_dragging)
        layout->SetExternalDockHint(false);
}

void FloatingPanelManager::DrawSurfaces(const std::shared_ptr<RHI>& rhi)
{
#if defined(_WIN32)
    if (rhi == nullptr || m_Panels.empty() || rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
        return;

    DX12RHI* dx12 = static_cast<DX12RHI*>(rhi.get());
    ZSlate::ZSlateEditorOverlay& overlay = ZSlate::ZSlateEditorOverlay::Get();
    const float clear_color[4] = {0.15f, 0.15f, 0.15f, 1.0f};

    for (auto& panel_ptr : m_Panels)
    {
        FloatingPanel& panel = *panel_ptr;
        if (panel.surface == nullptr || panel.width <= 0 || panel.height <= 0)
            continue;

        dx12->BeginFloatingSurfaceDraw(panel.surface, clear_color);
        if (panel.batch_ready)
        {
            overlay.DrawExternalBatchToFloatingSurface(rhi,
                                                       panel.surface,
                                                       panel.renderer.getBatch(),
                                                       static_cast<uint32_t>(panel.width),
                                                       static_cast<uint32_t>(panel.height));
        }
        dx12->EndFloatingSurfaceDraw(panel.surface);
    }
#else
    (void)rhi;
#endif
}

void FloatingPanelManager::Shutdown()
{
    // Persist the live floating set BEFORE destroying windows so the next session
    // restores it (DestroyPanel below uses redock=false so the dock tree the editor
    // saves does not also contain these panels -- the two stores stay disjoint).
    SaveState();
    for (auto& panel_ptr : m_Panels)
        DestroyPanel(*panel_ptr, /*redock=*/false);
    m_Panels.clear();
    m_PendingFloat.clear();
    m_PendingDock.clear();
    m_EditorUi = nullptr;
}

namespace
{
    std::filesystem::path FloatingPanelsFilePath()
    {
        auto project = GET_SYSTEM(ProjectInfo);
        if (project == nullptr)
            return {};
        std::filesystem::path root = project->GetProjectRoot();
        if (root.empty())
            root = std::filesystem::current_path();
        const std::string saved = project->saved_dir.empty() ? std::string("saved") : project->saved_dir;
        return root / saved / "config" / "floating_panels.json";
    }
}  // namespace

void FloatingPanelManager::SaveState() const
{
    const std::filesystem::path file_path = FloatingPanelsFilePath();
    if (file_path.empty())
        return;

    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& alloc = doc.GetAllocator();
    doc.AddMember("version", 1, alloc);

    rapidjson::Value panels(rapidjson::kArrayType);
    for (const auto& panel_ptr : m_Panels)
    {
        const FloatingPanel& panel = *panel_ptr;
        int wx = panel.win_x;
        int wy = panel.win_y;
        int ww = panel.width;
        int wh = panel.height;
#if defined(_WIN32)
        if (panel.window != nullptr)
        {
            glfwGetWindowPos(panel.window, &wx, &wy);
            // Logical (screen-coord) size -- that is what CreateChildWindow expects.
            glfwGetWindowSize(panel.window, &ww, &wh);
        }
#endif
        rapidjson::Value entry(rapidjson::kObjectType);
        rapidjson::Value title;
        title.SetString(panel.title.c_str(), static_cast<rapidjson::SizeType>(panel.title.size()), alloc);
        entry.AddMember("title", title, alloc);
        entry.AddMember("x", wx, alloc);
        entry.AddMember("y", wy, alloc);
        entry.AddMember("w", ww, alloc);
        entry.AddMember("h", wh, alloc);
        panels.PushBack(entry, alloc);
    }
    doc.AddMember("panels", panels, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    try
    {
        std::filesystem::create_directories(file_path.parent_path());
        const std::filesystem::path tmp_path = file_path.string() + ".tmp";
        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
                return;
            file << buffer.GetString();
        }
        std::filesystem::rename(tmp_path, file_path);
    }
    catch (const std::exception&)
    {
        // Persistence is best-effort; a failed write just means no restore next run.
    }
}

void FloatingPanelManager::LoadState()
{
    const std::filesystem::path file_path = FloatingPanelsFilePath();
    if (file_path.empty() || !std::filesystem::exists(file_path))
        return;

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open())
        return;
    rapidjson::IStreamWrapper stream_wrapper(file);
    rapidjson::Document doc;
    doc.ParseStream(stream_wrapper);
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("panels") || !doc["panels"].IsArray())
        return;

    for (const rapidjson::Value& entry : doc["panels"].GetArray())
    {
        if (!entry.IsObject() || !entry.HasMember("title") || !entry["title"].IsString())
            continue;
        const std::string title = entry["title"].GetString();
        const int x = entry.HasMember("x") && entry["x"].IsInt() ? entry["x"].GetInt() : 120;
        const int y = entry.HasMember("y") && entry["y"].IsInt() ? entry["y"].GetInt() : 120;
        const int w = entry.HasMember("w") && entry["w"].IsInt() ? entry["w"].GetInt() : 900;
        const int h = entry.HasMember("h") && entry["h"].IsInt() ? entry["h"].GetInt() : 600;
        RequestFloatAt(title, x, y, w, h);
    }
}
