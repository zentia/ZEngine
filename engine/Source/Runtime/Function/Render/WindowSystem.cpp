#include "WindowSystem.h"

#include "Application/Application.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Resource/Config/ConfigManager.h"

// -----------------------------------------------------------------------------
// On Emscripten, GLFW is implemented as a shim (-sUSE_GLFW=3) on top of the
// browser's <canvas> + DOM events. The shim covers the GLFW core entry points
// (init / createWindow / set*Callback / pollEvents / windowShouldClose /
// getMouseButton / setInputMode(GLFW_CURSOR, ...)) but DOES NOT honor a
// number of desktop-only calls — most notably:
//   * GLFW_RAW_MOUSE_MOTION input mode (no concept on the Web)
//   * glfwMaximizeWindow / glfwHideWindow / glfwShowWindow / glfwSetWindowTitle
//     against the browser frame
//   * glfwGetWindowMonitor (no monitor abstraction)
//   * glfwDestroyWindow / glfwTerminate against a canvas (asserts at runtime)
//
// We therefore #if-gate the calls that the shim cannot honor and route the
// Web build through GLFW_OPENGL_ES_API + version 3.0 hints so the shim
// internally creates a WebGL2 context against the canvas. WebGL2RHI then
// picks up that already-current context via emscripten_webgl_get_current_context().
// -----------------------------------------------------------------------------

#if defined(__EMSCRIPTEN__)
    #include <emscripten/emscripten.h>
    #include <emscripten/html5.h>
#endif

std::vector<std::type_index> WindowSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(ConfigManager)};
}

void WindowSystem::Shutdown()
{
    GET_SYSTEM(UserPreferences)->SetInt("Window.Width", m_Width);
    GET_SYSTEM(UserPreferences)->SetInt("Window.Height", m_Height);

#if !defined(__EMSCRIPTEN__)
    GLFWmonitor* monitor = glfwGetWindowMonitor(m_Window);
    bool is_fullscreen = (monitor != nullptr);
    GET_SYSTEM(UserPreferences)->SetBool("Window.IsFullscreen", is_fullscreen);
    glfwDestroyWindow(m_Window);
    glfwTerminate();
#else
    // Browser: the canvas is owned by the page; do NOT destroy/terminate.
    // We just record the last known fullscreen state for parity with desktop.
    GET_SYSTEM(UserPreferences)->SetBool("Window.IsFullscreen", false);
#endif
}

bool WindowSystem::Initialize()
{
    WindowCreateInfo window_create_info;
    window_create_info.is_fullscreen = GET_SYSTEM(UserPreferences)->GetBool("Window.IsFullscreen", true);
    window_create_info.title = GET_SYSTEM(Application)->name.c_str();
    if (!glfwInit())
    {
        LOG_FATAL(ZWindow, "failed to initialize GLFW {}", __FUNCTION__);
        return false;
    }

    m_Width = Math::max(1280, GET_SYSTEM(UserPreferences)->GetInt("Window.Width", 1280));
    m_Height = Math::max(720, GET_SYSTEM(UserPreferences)->GetInt("Window.Height", 720));

#if defined(__EMSCRIPTEN__)
    // Browser path: ask the GLFW shim to create a WebGL 2.0 context against
    // the default canvas. The shim translates these hints into an
    // EmscriptenWebGLContextAttributes call internally.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
#else
    // Desktop path: RHI owns the GL/Vulkan/DX12/Metal context, GLFW only
    // provides the platform window.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif

    m_Window =
        glfwCreateWindow(m_Width, m_Height, GET_SYSTEM(PlayerSettings)->m_ProjectName.c_str(), nullptr, nullptr);
    if (!m_Window)
    {
        LOG_FATAL(ZWindow, "failed to create window {}", __FUNCTION__);
#if !defined(__EMSCRIPTEN__)
        glfwTerminate();
#endif
        return false;
    }

    // Setup input callbacks
    glfwSetWindowUserPointer(m_Window, this);
    glfwSetKeyCallback(m_Window, keyCallback);
    glfwSetCharCallback(m_Window, charCallback);
#if !defined(__EMSCRIPTEN__)
    // Emscripten's GLFW shim does not implement glfwSetCharModsCallback and
    // calling it aborts the runtime ("glfwSetCharModsCallback not implemented.").
    // The browser's keyboard input is fully covered by SetKeyCallback +
    // SetCharCallback, so the modifier-aware char callback is desktop-only.
    glfwSetCharModsCallback(m_Window, charModsCallback);
#endif
    glfwSetMouseButtonCallback(m_Window, mouseButtonCallback);
    glfwSetCursorPosCallback(m_Window, cursorPosCallback);
    glfwSetCursorEnterCallback(m_Window, cursorEnterCallback);
    glfwSetScrollCallback(m_Window, scrollCallback);
    glfwSetDropCallback(m_Window, dropCallback);
    glfwSetWindowSizeCallback(m_Window, windowSizeCallback);
    glfwSetWindowCloseCallback(m_Window, windowCloseCallback);
    // Drives a synchronous live redraw during the Win32 modal resize/move loop (see
    // onWindowRefreshFunc docs). Without this the FLIP swapchain shows white while a border drag
    // is held (the normal Run() loop is blocked in glfwPollEvents and never presents).
    glfwSetWindowRefreshCallback(m_Window, windowRefreshCallback);
    // PR-AI3: window-focus callback drives EditorAssetManager's
    // AutoReimport scan on focus-gained.
    glfwSetWindowFocusCallback(m_Window, windowFocusCallback);

#if !defined(__EMSCRIPTEN__)
    // Raw mouse motion is a desktop-only input mode; the browser exposes
    // movementX/Y deltas via the Pointer Lock API which the shim already
    // routes through cursorPosCallback when GLFW_CURSOR is DISABLED.
    glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    if (GET_SYSTEM(UserPreferences)->GetBool("Window.IsFullscreen", false))
        glfwMaximizeWindow(m_Window);

    // Hide main window before showing splash screen (no splash on Web).
    glfwHideWindow(m_Window);
#else
    // On Web the canvas is always visible to the user as soon as the page
    // loads; size/fullscreen are page-level decisions handled in JS.
    // Sync our drawable size to the actual canvas buffer size so RHI can
    // size its viewport correctly.
    int canvas_w = 0;
    int canvas_h = 0;
    if (emscripten_get_canvas_element_size("#canvas", &canvas_w, &canvas_h) == EMSCRIPTEN_RESULT_SUCCESS)
    {
        if (canvas_w > 0)
            m_Width = canvas_w;
        if (canvas_h > 0)
            m_Height = canvas_h;
    }
#endif

    glfwPollEvents();
    return true;
}

void WindowSystem::PollEvents() const
{
    glfwPollEvents();
}

GLFWwindow* WindowSystem::CreateChildWindow(const char* title, int width, int height, int pos_x, int pos_y,
                                            bool decorated)
{
#if defined(__EMSCRIPTEN__)
    (void)title;
    (void)width;
    (void)height;
    (void)pos_x;
    (void)pos_y;
    (void)decorated;
    return nullptr;
#else
    // Mirror the main window's desktop hints: the RHI owns the graphics surface,
    // so GLFW must not create a GL context. Hidden-until-shown. `decorated` is
    // false for editor tear-off windows (custom Slate chrome, UE model).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);

    width = Math::max(width, 1);
    height = Math::max(height, 1);
    GLFWwindow* window = glfwCreateWindow(width, height, title != nullptr ? title : "Panel", nullptr, nullptr);
    if (window == nullptr)
    {
        LOG_ERROR(ZWindow, "WindowSystem::CreateChildWindow failed ({}x{})", width, height);
        return nullptr;
    }
    glfwSetWindowPos(window, pos_x, pos_y);
    return window;
#endif
}

void WindowSystem::DestroyChildWindow(GLFWwindow* window)
{
#if !defined(__EMSCRIPTEN__)
    if (window != nullptr && window != m_Window)
    {
        glfwDestroyWindow(window);
    }
#endif
}

void WindowSystem::RequestClose()
{
#if !defined(__EMSCRIPTEN__)
    if (m_Window)
    {
        glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    }
#endif
}

bool WindowSystem::ShouldClose() const
{
#if defined(__EMSCRIPTEN__)
    // The browser drives the main loop via requestAnimationFrame; there is
    // no "close" event delivered to wasm. The frame loop is torn down when
    // the user navigates away, so from the engine's point of view the
    // window is never closed.
    return false;
#else
    return glfwWindowShouldClose(m_Window);
#endif
}

void WindowSystem::SetTitle(const char* title)
{
#if defined(__EMSCRIPTEN__)
    // The page <title> is owned by the embedding HTML; mutating it from
    // wasm via glfwSetWindowTitle is a no-op in the shim. Skip cleanly.
    (void)title;
#else
    glfwSetWindowTitle(m_Window, title);
#endif
}

GLFWwindow* WindowSystem::GetWindow() const
{
    return m_Window;
}

std::array<int, 2> WindowSystem::GetWindowSize() const
{
    return std::array<int, 2>({m_Width, m_Height});
}

std::array<int, 2> WindowSystem::GetFramebufferSize() const
{
    if (m_Window == nullptr)
    {
        return {m_Width, m_Height};
    }

    int framebuffer_width = m_Width;
    int framebuffer_height = m_Height;
    glfwGetFramebufferSize(m_Window, &framebuffer_width, &framebuffer_height);
    framebuffer_width = Math::max(framebuffer_width, 1);
    framebuffer_height = Math::max(framebuffer_height, 1);
    return {framebuffer_width, framebuffer_height};
}

void WindowSystem::SetFocusMode(bool mode)
{
    m_IsFocusMode = mode;
    glfwSetInputMode(m_Window, GLFW_CURSOR, m_IsFocusMode ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void WindowSystem::ShowWindow()
{
#if defined(__EMSCRIPTEN__)
    // Canvas is already visible on the page; nothing to do.
    return;
#else
    // Show main window after splash screen is closed
    if (m_Window)
    {
        glfwShowWindow(m_Window);
    }
#endif
}
