#include "WindowSystem.h"

#include "Application/Application.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#if Z_PLATFORM_WINDOWS
    #include "Platform/Windows/WindowsApplication.h"
#endif

// ============================================================================
// 平台应用工厂
// ============================================================================
static std::unique_ptr<GenericApplication> CreatePlatformApplication()
{
#if Z_PLATFORM_WINDOWS
    return std::make_unique<WindowsApplication>();
#elif Z_PLATFORM_MACOS || Z_PLATFORM_IOS || Z_PLATFORM_ANDROID || Z_PLATFORM_OHOS
    // macOS / iOS / Android / OHOS: window / view is owned by the host
    // (Cocoa NSWindow / UIKit UIView / Android Surface / OHOS XComponent).
    // The platform RHI attaches to an externally-provided layer/surface at runtime.
    // TODO: Implement per-platform standalone window management.
    return nullptr;
#else
    #error "CreatePlatformApplication: unsupported platform"
#endif
}

// ============================================================================
// WindowSystem 实现
// ============================================================================

std::vector<std::type_index> WindowSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(ConfigManager)};
}

void WindowSystem::Shutdown()
{
    auto* user_prefs = SystemRegistry::GetInstance().GetSystem<UserPreferences>();
    if (user_prefs && m_Application && m_Application->GetMainWindow())
    {
        auto size = m_Application->GetMainWindow()->GetSize();
        user_prefs->SetInt("Window.Width", size[0]);
        user_prefs->SetInt("Window.Height", size[1]);
    }

    m_Application.reset();
    m_SplashScreen = nullptr;
}

bool WindowSystem::Initialize()
{
    auto* user_prefs   = SystemRegistry::GetInstance().GetSystem<UserPreferences>();
    auto* app           = SystemRegistry::GetInstance().GetSystem<Application>();

    const int width  = std::max(1280, user_prefs ? user_prefs->GetInt("Window.Width", 1280) : 1280);
    const int height = std::max(720,  user_prefs ? user_prefs->GetInt("Window.Height", 720) : 720);

    m_Application = CreatePlatformApplication();

#if Z_PLATFORM_MACOS || Z_PLATFORM_IOS || Z_PLATFORM_ANDROID || Z_PLATFORM_OHOS
    // Apple / Android / OHOS platforms: window is managed externally by the host.
    // The platform RHI attaches to the host's layer/surface; WindowSystem provides
    // only the input/event shell — window creation is deferred to the host.
    if (!m_Application)
    {
        return true;
    }
#endif

    if (!m_Application->Initialize(app->name.c_str(), width, height))
    {
        LOG_FATAL(ZWindow, "failed to create platform window {}", __FUNCTION__);
        return false;
    }

    // ---- 注册回调转发（主窗口 GenericWindow -> WindowSystem::OnXxx） ----
    auto* mainWnd = m_Application->GetMainWindow();
    if (mainWnd)
    {
        mainWnd->OnKey.push_back(       [this](int k, int s, int a, int m)       { OnKey(k, s, a, m); });
        mainWnd->OnChar.push_back(     [this](unsigned int c)                          { OnChar(c); });
        mainWnd->OnCharMods.push_back([this](int c, unsigned int m)                { OnCharMods(c, m); });
        mainWnd->OnMouseButton.push_back([this](int b, int a, int m)              { OnMouseButton(b, a, m); });
        mainWnd->OnCursorPos.push_back( [this](double x, double y)                { OnCursorPos(x, y); });
        mainWnd->OnCursorEnter.push_back([this](int e)                             { OnCursorEnter(e); });
        mainWnd->OnScroll.push_back(    [this](double x, double y)                { OnScroll(x, y); });
        mainWnd->OnDrop.push_back(      [this](int c, const char** p)             { OnDrop(c, p); });
        mainWnd->OnWindowSize.push_back([this](int w, int h)                       { OnWindowSize(w, h); });
        mainWnd->OnWindowClose.push_back([this]() { RequestClose(); });
        mainWnd->OnWindowFocus.push_back([this](int f)                           { OnWindowFocus(f); });
        mainWnd->OnWindowRefresh.push_back([this]() { OnWindowRefresh(); });
    }

    m_Application->PollEvents();
    return true;
}

void WindowSystem::PollEvents() const
{
    if (m_Application) m_Application->PollEvents();
}

bool WindowSystem::ShouldClose() const
{
    return m_Application && m_Application->ShouldClose();
}

void WindowSystem::RequestClose()
{
    if (m_Application) m_Application->RequestClose();
}

void WindowSystem::SetTitle(const char* title)
{
    if (m_Application) m_Application->SetTitle(title);
}

GenericWindow* WindowSystem::GetMainWindow() const
{
    return m_Application ? m_Application->GetMainWindow() : nullptr;
}

void* WindowSystem::GetNativeWindowHandle() const
{
    if (!m_Application || !m_Application->GetMainWindow()) return nullptr;
    return m_Application->GetMainWindow()->GetNativeHandle();
}

GenericWindow* WindowSystem::CreateChildWindow(const char* title,
                                               int        width,
                                               int        height,
                                               int        pos_x,
                                               int        pos_y,
                                               bool       decorated)
{
    if (!m_Application) return nullptr;
    return m_Application->CreateChildWindow(title, width, height, pos_x, pos_y, decorated);
}

void WindowSystem::DestroyChildWindow(GenericWindow* window)
{
    if (m_Application) m_Application->DestroyChildWindow(window);
}

std::array<int, 2> WindowSystem::GetWindowSize() const
{
    if (!m_Application || !m_Application->GetMainWindow()) return {1280, 720};
    return m_Application->GetMainWindow()->GetSize();
}

std::array<int, 2> WindowSystem::GetFramebufferSize() const
{
    if (!m_Application) return {1280, 720};
    return m_Application->GetFramebufferSize();
}

void WindowSystem::ShowWindow()
{
    if (m_Application) m_Application->ShowMainWindow();
}

bool WindowSystem::IsMouseButtonDown(int button) const
{
    if (!m_Application || !m_Application->GetMainWindow()) return false;
    // GenericWindow 暂不跟踪鼠标状态；用平台 API 查询
#if Z_PLATFORM_WINDOWS
    int vk = -1;
    switch (button)
    {
    case 0: vk = VK_LBUTTON; break;
    case 1: vk = VK_RBUTTON; break;
    case 2: vk = VK_MBUTTON; break;
    default: return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    return false;
#endif
}

void WindowSystem::SetFocusMode(bool mode)
{
    m_IsFocusMode = mode;
    if (m_Application) m_Application->SetCursorMode(mode);
}

// ============================================================================
// 回调转发（由 GenericWindow 的 OnXxx 调用）
// ============================================================================

void WindowSystem::OnReset()
{
    for (auto& func : m_OnResetFunc) func();
}

void WindowSystem::OnKey(int key, int scancode, int action, int mods)
{
    if (m_Application) m_Application->UpdateKeyState(key, action);
    for (auto& func : m_OnKeyFunc) func(key, scancode, action, mods);
}

void WindowSystem::OnChar(unsigned int codepoint)
{
    for (auto& func : m_OnCharFunc) func(codepoint);
}

void WindowSystem::OnCharMods(int codepoint, unsigned int mods)
{
    for (auto& func : m_OnCharModsFunc) func(codepoint, mods);
}

void WindowSystem::OnMouseButton(int button, int action, int mods)
{
    if (m_Application) m_Application->UpdateMouseState(button, action);
    for (auto& func : m_OnMouseButtonFunc) func(button, action, mods);
}

void WindowSystem::OnCursorPos(double xpos, double ypos)
{
    for (auto& func : m_OnCursorPosFunc) func(xpos, ypos);
}

void WindowSystem::OnCursorEnter(int entered)
{
    for (auto& func : m_OnCursorEnterFunc) func(entered);
}

void WindowSystem::OnScroll(double xoffset, double yoffset)
{
    for (auto& func : m_OnScrollFunc) func(xoffset, yoffset);
}

void WindowSystem::OnDrop(int count, const char** paths)
{
    for (auto& func : m_OnDropFunc) func(count, paths);
}

void WindowSystem::OnWindowSize(int width, int height)
{
    if (auto* prefs = SystemRegistry::GetInstance().GetSystem<UserPreferences>())
    {
        prefs->SetInt("Window.Width", width);
        prefs->SetInt("Window.Height", height);
        prefs->save();
    }
    for (auto& func : m_OnWindowSizeFunc) func(width, height);
}

void WindowSystem::OnWindowFocus(int focused)
{
    for (auto& func : m_OnWindowFocusFunc) func(focused);
}

void WindowSystem::OnWindowRefresh()
{
    for (auto& func : m_OnWindowRefreshFunc) func();
}
