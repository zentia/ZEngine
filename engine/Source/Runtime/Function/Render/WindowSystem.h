#pragma once

#if defined(Z_HAS_VULKAN)
    #define GLFW_INCLUDE_VULKAN
#else
    #define GLFW_INCLUDE_NONE
#endif
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"

#include <GLFW/glfw3.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SplashScreen;

class WindowCreateInfo
{
public:
    const char* title {"Z"};
    bool is_fullscreen {true};
};

class WindowSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "WindowSystem"; }
    std::vector<std::type_index> GetDependencies() const override;
    void Shutdown() override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Platform; }
    bool Initialize() override;
    WindowSystem() = default;

    void PollEvents() const;
    bool ShouldClose() const;
    void RequestClose();
    void SetTitle(const char* title);
    GLFWwindow* GetWindow() const;

    // Secondary (child) OS windows for torn-off editor panels. Desktop-only --
    // returns nullptr on the Web (single canvas). The returned window is created
    // decorated and INITIALLY HIDDEN with the same GLFW_NO_API hint as the main
    // window (the RHI owns the graphics surface). The CALLER owns input: it should
    // glfwSetWindowUserPointer() + install its own GLFW callbacks on the handle,
    // and call glfwShowWindow() once its first frame is ready. glfwPollEvents()
    // (driven by the editor's single PollEvents) dispatches events for every live
    // GLFW window, including these, so no extra pump is needed.
    // `decorated` controls the OS title bar / borders. Editor tear-off windows pass
    // false: they draw their own title bar (so the tab drag is handled in-app and the
    // window can be re-docked without fighting the OS modal move loop, UE-style).
    GLFWwindow* CreateChildWindow(const char* title, int width, int height, int pos_x, int pos_y,
                                  bool decorated = true);
    void DestroyChildWindow(GLFWwindow* window);
    std::array<int, 2> GetWindowSize() const;
    // GLFW framebuffer pixels (matches DXGI swapchain / D3D12 viewport space).
    std::array<int, 2> GetFramebufferSize() const;
    void ShowWindow();

    typedef std::function<void()> onResetFunc;
    typedef std::function<void(int, int, int, int)> onKeyFunc;
    typedef std::function<void(unsigned int)> onCharFunc;
    typedef std::function<void(int, unsigned int)> onCharModsFunc;
    typedef std::function<void(int, int, int)> onMouseButtonFunc;
    typedef std::function<void(double, double)> onCursorPosFunc;
    typedef std::function<void(int)> onCursorEnterFunc;
    typedef std::function<void(double, double)> onScrollFunc;
    typedef std::function<void(int, const char**)> onDropFunc;
    typedef std::function<void(int, int)> onWindowSizeFunc;
    typedef std::function<void()> onWindowCloseFunc;
    // PR-AI3: GLFW window focus callback. `focused` is GLFW_TRUE (1) when
    // the window gains input focus (Alt-Tab back from another app, or
    // user clicking the title bar) and GLFW_FALSE (0) on focus loss.
    // EditorAssetManager subscribes to drive AutoReimport: on focus
    // gained we re-stat every recorded source file in
    // SourceAssetRegistry. Implementation is symmetrical to all other
    // onXxxFunc callbacks here -- one std::vector + a static GLFW shim
    // that fans out to subscribers.
    typedef std::function<void(int /*focused*/)> onWindowFocusFunc;
    // GLFW window-refresh callback. GLFW fires this from inside glfwPollEvents whenever the OS
    // requests a repaint -- crucially this includes WM_PAINT delivered DURING the Win32 modal
    // resize/move loop, when glfwPollEvents would otherwise block and freeze the render loop.
    // The editor subscribes here to synchronously render one frame at the live size so a border
    // drag keeps presenting (FLIP-model swapchains show white for the un-presented grown area).
    typedef std::function<void()> onWindowRefreshFunc;

    void registerOnResetFunc(onResetFunc func) { m_Onresetfunc.push_back(func); }
    void registerOnKeyFunc(onKeyFunc func) { m_Onkeyfunc.push_back(func); }
    void registerOnCharFunc(onCharFunc func) { m_Oncharfunc.push_back(func); }
    void registerOnCharModsFunc(onCharModsFunc func) { m_Oncharmodsfunc.push_back(func); }
    void registerOnMouseButtonFunc(onMouseButtonFunc func) { m_Onmousebuttonfunc.push_back(func); }
    void registerOnCursorPosFunc(onCursorPosFunc func) { m_Oncursorposfunc.push_back(func); }
    void registerOnCursorEnterFunc(onCursorEnterFunc func) { m_Oncursorenterfunc.push_back(func); }
    void registerOnScrollFunc(onScrollFunc func) { m_Onscrollfunc.push_back(func); }
    void registerOnDropFunc(onDropFunc func) { m_Ondropfunc.push_back(func); }
    void registerOnWindowSizeFunc(onWindowSizeFunc func) { m_Onwindowsizefunc.push_back(func); }
    void registerOnWindowCloseFunc(onWindowCloseFunc func) { m_Onwindowclosefunc.push_back(func); }
    void registerOnWindowFocusFunc(onWindowFocusFunc func) { m_Onwindowfocusfunc.push_back(func); }
    void registerOnWindowRefreshFunc(onWindowRefreshFunc func) { m_Onwindowrefreshfunc.push_back(func); }

    bool isMouseButtonDown(int button) const
    {
        if (button < GLFW_MOUSE_BUTTON_1 || button > GLFW_MOUSE_BUTTON_LAST)
        {
            return false;
        }
        return glfwGetMouseButton(m_Window, button) == GLFW_PRESS;
    }
    bool getFocusMode() const { return m_IsFocusMode; }
    void SetFocusMode(bool mode);

protected:
    // window event callbacks
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->OnKey(key, scancode, action, mods);
        }
    }
    static void charCallback(GLFWwindow* window, unsigned int codepoint)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->OnChar(codepoint);
        }
    }
    static void charModsCallback(GLFWwindow* window, unsigned int codepoint, int mods)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->onCharMods(codepoint, mods);
        }
    }
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->onMouseButton(button, action, mods);
        }
    }
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->OnCursorPos(xpos, ypos);
        }
    }
    static void cursorEnterCallback(GLFWwindow* window, int entered)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->OnCursorEnter(entered);
        }
    }
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->OnScroll(xoffset, yoffset);
        }
    }
    static void dropCallback(GLFWwindow* window, int count, const char** paths)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->onDrop(count, paths);
        }
    }
    static void windowSizeCallback(GLFWwindow* window, int width, int height)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->m_Width = width;
            app->m_Height = height;
            GET_SYSTEM(UserPreferences)->SetInt("Window.Width", width);
            GET_SYSTEM(UserPreferences)->SetInt("Window.Height", height);
            GET_SYSTEM(UserPreferences)->save();
            // Dispatch to registered listeners. Without this, every
            // registerOnWindowSizeFunc subscriber (currently EditorSlateHost, which
            // owns the editor UI's display size used as the overlay NDC divisor) is
            // dead and its size stays frozen at its init value while the swapchain
            // tracks the real window -- scaling the whole editor UI by fb/display.
            app->onWindowSize(width, height);
        }
    }
    static void windowCloseCallback(GLFWwindow* window) { glfwSetWindowShouldClose(window, true); }
    // Fires during the Win32 modal resize/move loop (via WM_PAINT) as well as on ordinary expose
    // events. Lets subscribers keep rendering while glfwPollEvents is otherwise blocked.
    static void windowRefreshCallback(GLFWwindow* window)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->onWindowRefresh();
        }
    }

    // PR-AI3: GLFW window-focus shim. GLFW invokes this with focused=1
    // on focus gained and focused=0 on focus lost. We forward verbatim
    // to subscribers so they can distinguish the two states (today
    // EditorAssetManager only acts on focus-gained, but a future
    // power-saving path might want focus-lost too).
    static void windowFocusCallback(GLFWwindow* window, int focused)
    {
        WindowSystem* app = (WindowSystem*)glfwGetWindowUserPointer(window);
        if (app)
        {
            app->onWindowFocus(focused);
        }
    }

    void OnReset()
    {
        for (auto& func : m_Onresetfunc)
            func();
    }
    void OnKey(int key, int scancode, int action, int mods)
    {
        for (auto& func : m_Onkeyfunc)
            func(key, scancode, action, mods);
    }
    void OnChar(unsigned int codepoint)
    {
        for (auto& func : m_Oncharfunc)
            func(codepoint);
    }
    void onCharMods(int codepoint, unsigned int mods)
    {
        for (auto& func : m_Oncharmodsfunc)
            func(codepoint, mods);
    }
    void onMouseButton(int button, int action, int mods)
    {
        for (auto& func : m_Onmousebuttonfunc)
            func(button, action, mods);
    }
    void OnCursorPos(double xpos, double ypos)
    {
        for (auto& func : m_Oncursorposfunc)
            func(xpos, ypos);
    }
    void OnCursorEnter(int entered)
    {
        for (auto& func : m_Oncursorenterfunc)
            func(entered);
    }
    void OnScroll(double xoffset, double yoffset)
    {
        for (auto& func : m_Onscrollfunc)
            func(xoffset, yoffset);
    }
    void onDrop(int count, const char** paths)
    {
        for (auto& func : m_Ondropfunc)
            func(count, paths);
    }
    void onWindowSize(int width, int height)
    {
        for (auto& func : m_Onwindowsizefunc)
            func(width, height);
    }
    // PR-AI3: fanout for the window-focus callback. Same shape as
    // onDrop / onWindowSize.
    void onWindowFocus(int focused)
    {
        for (auto& func : m_Onwindowfocusfunc)
            func(focused);
    }
    void onWindowRefresh()
    {
        for (auto& func : m_Onwindowrefreshfunc)
            func();
    }

private:
    GLFWwindow* m_Window {nullptr};
    int m_Width {0};
    int m_Height {0};

    bool m_IsFocusMode {false};

    SplashScreen* m_SplashScreen;

    std::vector<onResetFunc> m_Onresetfunc;
    std::vector<onKeyFunc> m_Onkeyfunc;
    std::vector<onCharFunc> m_Oncharfunc;
    std::vector<onCharModsFunc> m_Oncharmodsfunc;
    std::vector<onMouseButtonFunc> m_Onmousebuttonfunc;
    std::vector<onCursorPosFunc> m_Oncursorposfunc;
    std::vector<onCursorEnterFunc> m_Oncursorenterfunc;
    std::vector<onScrollFunc> m_Onscrollfunc;
    std::vector<onDropFunc> m_Ondropfunc;
    std::vector<onWindowSizeFunc> m_Onwindowsizefunc;
    std::vector<onWindowCloseFunc> m_Onwindowclosefunc;
    std::vector<onWindowFocusFunc> m_Onwindowfocusfunc;
    std::vector<onWindowRefreshFunc> m_Onwindowrefreshfunc;
};