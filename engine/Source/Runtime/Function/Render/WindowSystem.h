#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Resource/UserPreferences/UserPreferences.h"
#include "Platform/Generic/GenericApplication.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SplashScreen;

// WindowSystem 是平台无关的窗口抽象，内部通过 GenericApplication
// 委托给平台特定实现（Win32 / Cocoa / X11）。
// 参考 UE 的 FGenericApplication / FWindowsApplication 模式。
class WindowSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "WindowSystem"; }
    std::vector<std::type_index> GetDependencies() const override;
    void Shutdown() override;
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Platform; }
    bool Initialize() override;
    WindowSystem() = default;

    // 消息泵，等价 glfwPollEvents()
    void PollEvents() const;

    bool ShouldClose() const;
    void RequestClose();
    void SetTitle(const char* title);

    // 主窗口访问
    GenericWindow* GetMainWindow() const;
    void* GetNativeWindowHandle() const;

    // 访问底层平台应用（剪贴板、光标、计时）
    GenericApplication* GetApplication() const { return m_Application.get(); }

    // 子窗口（编辑器浮窗）
    GenericWindow* CreateChildWindow(const char* title, int width, int height,
                                    int pos_x, int pos_y, bool decorated = true);
    void DestroyChildWindow(GenericWindow* window);

    std::array<int, 2> GetWindowSize() const;
    std::array<int, 2> GetFramebufferSize() const;
    void ShowWindow();

    // 回调注册（转发到主窗口的 GenericWindow 回调向量）
    typedef std::function<void()>                OnResetFunc;
    typedef std::function<void(int,int,int,int)> OnKeyFunc;
    typedef std::function<void(unsigned int)>    OnCharFunc;
    typedef std::function<void(int,unsigned int)> OnCharModsFunc;
    typedef std::function<void(int,int,int)>    OnMouseButtonFunc;
    typedef std::function<void(double,double)>    OnCursorPosFunc;
    typedef std::function<void(int)>                OnCursorEnterFunc;
    typedef std::function<void(double,double)>    OnScrollFunc;
    typedef std::function<void(int,const char**)> OnDropFunc;
    typedef std::function<void(int,int)>        OnWindowSizeFunc;
    typedef std::function<void()>                OnWindowCloseFunc;
    typedef std::function<void(int)>                OnWindowFocusFunc;
    typedef std::function<void()>                OnWindowRefreshFunc;

    void RegisterOnResetFunc(OnResetFunc func)          { m_OnResetFunc.push_back(func); }
    void RegisterOnKeyFunc(OnKeyFunc func)             { m_OnKeyFunc.push_back(func); }
    void RegisterOnCharFunc(OnCharFunc func)            { m_OnCharFunc.push_back(func); }
    void RegisterOnCharModsFunc(OnCharModsFunc func) { m_OnCharModsFunc.push_back(func); }
    void RegisterOnMouseButtonFunc(OnMouseButtonFunc func) { m_OnMouseButtonFunc.push_back(func); }
    void RegisterOnCursorPosFunc(OnCursorPosFunc func)    { m_OnCursorPosFunc.push_back(func); }
    void RegisterOnCursorEnterFunc(OnCursorEnterFunc func) { m_OnCursorEnterFunc.push_back(func); }
    void RegisterOnScrollFunc(OnScrollFunc func)          { m_OnScrollFunc.push_back(func); }
    void RegisterOnDropFunc(OnDropFunc func)              { m_OnDropFunc.push_back(func); }
    void RegisterOnWindowSizeFunc(OnWindowSizeFunc func)    { m_OnWindowSizeFunc.push_back(func); }
    void RegisterOnWindowCloseFunc(OnWindowCloseFunc func)  { m_OnWindowCloseFunc.push_back(func); }
    void RegisterOnWindowFocusFunc(OnWindowFocusFunc func)  { m_OnWindowFocusFunc.push_back(func); }
    void RegisterOnWindowRefreshFunc(OnWindowRefreshFunc func){ m_OnWindowRefreshFunc.push_back(func); }

    bool IsMouseButtonDown(int button) const;
    bool GetFocusMode() const { return m_IsFocusMode; }
    void SetFocusMode(bool mode);

private:
    // 回调转发（由 GenericWindow 回调调用）
    void OnReset();
    void OnKey(int key, int scancode, int action, int mods);
    void OnChar(unsigned int codepoint);
    void OnCharMods(int codepoint, unsigned int mods);
    void OnMouseButton(int button, int action, int mods);
    void OnCursorPos(double xpos, double ypos);
    void OnCursorEnter(int entered);
    void OnScroll(double xoffset, double yoffset);
    void OnDrop(int count, const char** paths);
    void OnWindowSize(int width, int height);
    void OnWindowFocus(int focused);
    void OnWindowRefresh();

    std::unique_ptr<GenericApplication> m_Application;
    bool                                   m_IsFocusMode{false};
    SplashScreen*                          m_SplashScreen{nullptr};

    // 主窗口回调向量（由 RegisterOnXxxFunc 注册）
    std::vector<OnResetFunc>         m_OnResetFunc;
    std::vector<OnKeyFunc>            m_OnKeyFunc;
    std::vector<OnCharFunc>           m_OnCharFunc;
    std::vector<OnCharModsFunc>      m_OnCharModsFunc;
    std::vector<OnMouseButtonFunc>     m_OnMouseButtonFunc;
    std::vector<OnCursorPosFunc>      m_OnCursorPosFunc;
    std::vector<OnCursorEnterFunc>     m_OnCursorEnterFunc;
    std::vector<OnScrollFunc>          m_OnScrollFunc;
    std::vector<OnDropFunc>            m_OnDropFunc;
    std::vector<OnWindowSizeFunc>      m_OnWindowSizeFunc;
    std::vector<OnWindowCloseFunc>     m_OnWindowCloseFunc;
    std::vector<OnWindowFocusFunc>     m_OnWindowFocusFunc;
    std::vector<OnWindowRefreshFunc>   m_OnWindowRefreshFunc;
};
