#include "Editor/ZSlate/Backend/EditorSlateHost.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ZSlate
{
namespace
{
    // Same mapping as the runtime UISystem (UISystem.cpp). Kept local because the
    // runtime helper lives in an anonymous namespace there; duplicating the small
    // switch is cheaper than exporting it.
    EKey MapGlfwKeyToSlate(int glfw_key)
    {
        switch (glfw_key)
        {
            case GLFW_KEY_BACKSPACE: return EKey::Backspace;
            case GLFW_KEY_DELETE:    return EKey::Delete;
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER:  return EKey::Enter;
            case GLFW_KEY_ESCAPE:    return EKey::Escape;
            case GLFW_KEY_LEFT:      return EKey::Left;
            case GLFW_KEY_RIGHT:     return EKey::Right;
            case GLFW_KEY_HOME:      return EKey::Home;
            case GLFW_KEY_END:       return EKey::End;
            case GLFW_KEY_SPACE:     return EKey::Space;
            default:                 return EKey::Unknown;
        }
    }
}  // namespace

EditorSlateHost& EditorSlateHost::Get()
{
    static EditorSlateHost instance;
    return instance;
}

bool EditorSlateHost::IsNativeInputEnabled()
{
    // P10c: the transitional r.ZSlate.NativeInput CVar has been retired. Native
    // ZSlate editor panels now unconditionally source input / scale / metrics /
    // hit-testing from the GLFW-backed EditorSlateHost and are hosted without an
    // ImGui::Begin (see EditorView::BeginGUI). This stays as a method (rather than
    // being inlined away at every call site) so the few remaining coexistence
    // gates read intentionally; it is a pure constant now.
    return true;
}

void EditorSlateHost::Initialize()
{
    if (m_Initialized)
    {
        return;
    }

    auto window = GET_SYSTEM(WindowSystem);
    if (window == nullptr)
    {
        return;
    }

    if (GLFWwindow* glfw_window = window->GetWindow())
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(glfw_window, &xs, &ys);
        m_UiScale = std::fmax(1.0f, std::fmax(xs, ys));
    }

    {
        const std::array<int, 2> size = window->GetWindowSize();
        m_WindowW = static_cast<float>(size[0]);
        m_WindowH = static_cast<float>(size[1]);
    }
    if (GLFWwindow* glfw_window = window->GetWindow())
    {
        int px = 0, py = 0;
        glfwGetWindowPos(glfw_window, &px, &py);
        m_WindowX = static_cast<float>(px);
        m_WindowY = static_cast<float>(py);
    }

    window->registerOnCursorPosFunc([this](double x, double y) {
        // Absolute screen coords = window-client origin + cursor offset, matching
        // ImGui io.MousePos under ViewportsEnable. glfwGetWindowPos is safe here
        // because cursor callbacks fire on the main thread during glfwPollEvents.
        int wx = 0, wy = 0;
        if (auto w = GET_SYSTEM(WindowSystem))
        {
            if (GLFWwindow* gw = w->GetWindow())
            {
                glfwGetWindowPos(gw, &wx, &wy);
            }
        }
        m_WindowX = static_cast<float>(wx);
        m_WindowY = static_cast<float>(wy);
        m_PointerScreen = Vector2(static_cast<float>(wx) + static_cast<float>(x),
                                  static_cast<float>(wy) + static_cast<float>(y));
    });

    window->registerOnMouseButtonFunc([this](int button, int action, int mods) {
        const bool down = (action != GLFW_RELEASE);
        if (button == GLFW_MOUSE_BUTTON_LEFT)
        {
            m_LeftDown = down;
            if (action == GLFW_PRESS)
            {
                // Accumulate a click edge (drained in NewFrame) so a press+release
                // within one UI frame is still seen, matching ImGui's event queue.
                ++m_PendingLeftPresses;
                // Double-click test against the previous press (ImGui defaults:
                // 0.30s / 6px). m_PointerScreen is current as cursor callbacks fire
                // before the button callback within the same glfwPollEvents pass.
                constexpr double kDoubleClickTime = 0.30;
                constexpr float kDoubleClickMaxDist = 6.0f;
                const double now = glfwGetTime();
                const float ddx = m_PointerScreen.x - m_LastLeftPressPos.x;
                const float ddy = m_PointerScreen.y - m_LastLeftPressPos.y;
                if (m_LastLeftPressTime >= 0.0 && (now - m_LastLeftPressTime) <= kDoubleClickTime &&
                    (ddx * ddx + ddy * ddy) <= (kDoubleClickMaxDist * kDoubleClickMaxDist))
                {
                    ++m_PendingLeftDoubleClicks;
                    // Reset so a triple-click is not counted as a second double.
                    m_LastLeftPressTime = -1.0;
                }
                else
                {
                    m_LastLeftPressTime = now;
                }
                m_LastLeftPressPos = m_PointerScreen;
            }
        }
        else if (button == GLFW_MOUSE_BUTTON_RIGHT)
        {
            m_RightDown = down;
        }
        else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
        {
            m_MiddleDown = down;
        }
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE)
        {
            ++m_PendingLeftReleases;
        }
        m_CtrlDown = (mods & GLFW_MOD_CONTROL) != 0;
        m_ShiftDown = (mods & GLFW_MOD_SHIFT) != 0;
        m_AltDown = (mods & GLFW_MOD_ALT) != 0;
    });

    window->registerOnScrollFunc([this](double /*xoff*/, double yoff) { m_PendingWheel += static_cast<float>(yoff); });

    window->registerOnKeyFunc([this](int key, int /*scan*/, int action, int mods) {
        m_CtrlDown = (mods & GLFW_MOD_CONTROL) != 0;
        m_ShiftDown = (mods & GLFW_MOD_SHIFT) != 0;
        m_AltDown = (mods & GLFW_MOD_ALT) != 0;
        if (action == GLFW_PRESS || action == GLFW_REPEAT)
        {
            const EKey k = MapGlfwKeyToSlate(key);
            if (k != EKey::Unknown)
            {
                m_PendingKeys.push_back(k);
            }
        }
    });

    window->registerOnCharFunc([this](unsigned int codepoint) { m_PendingChars.push_back(codepoint); });

    window->registerOnWindowSizeFunc([this](int w, int h) {
        m_WindowW = static_cast<float>(w);
        m_WindowH = static_cast<float>(h);
        // Maximize / restore moves the client origin too; refresh it here (fires
        // during glfwPollEvents on the main thread) so popup clamps stay aligned.
        if (auto win = GET_SYSTEM(WindowSystem))
        {
            if (GLFWwindow* gw = win->GetWindow())
            {
                int px = 0, py = 0;
                glfwGetWindowPos(gw, &px, &py);
                m_WindowX = static_cast<float>(px);
                m_WindowY = static_cast<float>(py);

                // Re-poll the DPI content scale on every size change. m_UiScale is
                // the SOLE DPI mechanism for the native editor: every font/metric in
                // the ZSlate windows is multiplied by it, and DefaultLayout re-solves
                // its geometry from it each frame, so an updated value re-lays out the
                // whole dock automatically. Seeding it once in Initialize() was wrong
                // -- GLFW commonly reports 1.0 before the window has settled on its
                // final monitor, and the scale also changes when the window is dragged
                // to a monitor with different scaling or the window is maximized onto a
                // high-DPI display. A stale 1.0 left fonts at their 16px design size
                // inside a full-resolution (e.g. 4K) surface: acceptable while small/
                // windowed, but tiny once maximized. This callback fires on the main
                // thread during glfwPollEvents (and on maximize / restore / WM_DPICHANGED),
                // so glfwGetWindowContentScale is safe to call here.
                float xs = 1.0f, ys = 1.0f;
                glfwGetWindowContentScale(gw, &xs, &ys);
                const float scale = std::fmax(1.0f, std::fmax(xs, ys));
                if (scale > 0.0f)
                {
                    m_UiScale = scale;
                }
            }
        }
    });

    if (GLFWwindow* glfw_window = window->GetWindow())
    {
        m_CursorHand = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
        m_CursorResizeEw = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
        m_CursorResizeNs = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
        m_CursorResizeNwse = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
        (void)glfw_window;
    }

    m_Initialized = true;
}

void EditorSlateHost::NewFrame()
{
    m_PointerDelta = Vector2(m_PointerScreen.x - m_PrevPointerScreen.x, m_PointerScreen.y - m_PrevPointerScreen.y);
    m_PrevPointerScreen = m_PointerScreen;

    m_FrameWheel = m_PendingWheel;
    m_PendingWheel = 0.0f;
    m_FrameChars = std::move(m_PendingChars);
    m_PendingChars.clear();
    m_FrameKeys = std::move(m_PendingKeys);
    m_PendingKeys.clear();

    m_FrameLeftPressed = (m_PendingLeftPresses > 0);
    m_PendingLeftPresses = 0;
    m_FrameLeftDoubleClicked = (m_PendingLeftDoubleClicks > 0);
    m_PendingLeftDoubleClicks = 0;
    m_FrameLeftReleased = (m_PendingLeftReleases > 0);
    m_PendingLeftReleases = 0;

    // Surfaces are re-registered every frame during panel paint. Keep the prior
    // frame's stack so HoveredSurfacePrev (the dock chrome gate, which runs before
    // panels register this frame) can hit-test a fully-populated set.
    m_SurfacesPrev = std::move(m_Surfaces);
    m_Surfaces.clear();
    m_NativeTextInputActive = false;
}

int EditorSlateHost::HashId(const char* name)
{
    // FNV-1a 32-bit. Returned as a non-negative int so -1 stays the "no hit"
    // sentinel from HoveredSurface (mask the sign bit).
    uint32_t h = 2166136261u;
    if (name != nullptr)
    {
        for (const char* p = name; *p != '\0'; ++p)
        {
            h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
            h *= 16777619u;
        }
    }
    return static_cast<int>(h & 0x7FFFFFFFu);
}

void EditorSlateHost::PushInputOverride(const FloatingInputState* state)
{
    m_Override = state;
    m_OverrideSurfaces.clear();
}

void EditorSlateHost::PopInputOverride()
{
    m_Override = nullptr;
    m_OverrideSurfaces.clear();
}

void EditorSlateHost::BeginSurface(int id, const UIRect& rect, ESurfaceLayer layer)
{
    Surface s {};
    s.id = id;
    s.rect = rect;
    s.layer = layer;
    // Tear-off: a floating window's panel paints into its own client space; route
    // its surfaces to the override stack so the main window's hit-test is untouched.
    if (m_Override != nullptr)
        m_OverrideSurfaces.push_back(s);
    else
        m_Surfaces.push_back(s);
}

int EditorSlateHost::HoveredIn(const std::vector<Surface>& surfaces, const Vector2& point) const
{
    int best_id = -1;
    int best_layer = -1;
    // Walk in registration order; ">=" on the layer comparison means a later
    // registration at the same layer wins the tie (stacking order = paint order).
    for (const Surface& s : surfaces)
    {
        if (!s.rect.Contains(point))
        {
            continue;
        }
        const int layer = static_cast<int>(s.layer);
        if (layer >= best_layer)
        {
            best_layer = layer;
            best_id = s.id;
        }
    }
    return best_id;
}

int EditorSlateHost::HoveredSurface(const Vector2& point) const
{
    // Tear-off: query the floating window's own stack while an override is active.
    return HoveredIn(m_Override != nullptr ? m_OverrideSurfaces : m_Surfaces, point);
}

int EditorSlateHost::HoveredSurfacePrev(const Vector2& point) const
{
    // Tear-off: floating windows have no separate dock-chrome pre-pass, so the
    // override's current-frame stack is the right answer while one is active.
    return HoveredIn(m_Override != nullptr ? m_OverrideSurfaces : m_SurfacesPrev, point);
}

bool EditorSlateHost::IsForegroundCapturing() const
{
    for (const Surface& s : (m_Override != nullptr ? m_OverrideSurfaces : m_Surfaces))
    {
        if (s.layer == ESurfaceLayer::Foreground)
        {
            return true;
        }
    }
    return false;
}

Vector2 EditorSlateHost::GetFramebufferScale() const
{
    auto window = GET_SYSTEM(WindowSystem);
    if (!window)
    {
        return Vector2(1.0f, 1.0f);
    }
    const std::array<int, 2> logical = window->GetWindowSize();
    const std::array<int, 2> framebuffer = window->GetFramebufferSize();
    const float scale_x = logical[0] > 0 ? static_cast<float>(framebuffer[0]) / static_cast<float>(logical[0]) : 1.0f;
    const float scale_y = logical[1] > 0 ? static_cast<float>(framebuffer[1]) / static_cast<float>(logical[1]) : 1.0f;
    return Vector2(scale_x, scale_y);
}

double EditorSlateHost::GetTime()
{
    return glfwGetTime();
}

void EditorSlateHost::SetMouseCursor(EMouseCursor cursor)
{
    GLFWwindow* glfw_window = nullptr;
    if (m_Override != nullptr && m_Override->window != nullptr)
    {
        // Tear-off: set the cursor on the floating window being painted. GLFW cursor
        // objects are shared across all windows in the process, so the main-window
        // cursors created in Initialize() apply here too.
        glfw_window = m_Override->window;
    }
    else if (auto window = GET_SYSTEM(WindowSystem))
    {
        glfw_window = window->GetWindow();
    }
    if (glfw_window == nullptr)
    {
        return;
    }
    GLFWcursor* glfw_cursor = nullptr;
    switch (cursor)
    {
        case EMouseCursor::Hand:       glfw_cursor = m_CursorHand; break;
        case EMouseCursor::ResizeEW:   glfw_cursor = m_CursorResizeEw; break;
        case EMouseCursor::ResizeNS:   glfw_cursor = m_CursorResizeNs; break;
        case EMouseCursor::ResizeNWSE: glfw_cursor = m_CursorResizeNwse; break;
        case EMouseCursor::Default:
        default:                       break;
    }
    glfwSetCursor(glfw_window, glfw_cursor);
}
}  // namespace ZSlate
