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

    // ---- 宿主提供的绘制表面（移动端 / 嵌入宿主）--------------------------
    //
    // 没有引擎自有的窗口时（CreatePlatformApplication() 返回 nullptr），surface
    // 由宿主推入：Android 是 ANativeWindow*，iOS 是 UIView*。RHI 建 surface /
    // layer 时从这里取，而不是从 GenericWindow 取 —— 后者在这些平台上不存在。
    //
    // 移动端的时间顺序值得注意：窗口往往晚于引擎启动才到（Android 的
    // surfaceCreated 回调），所以这是个"可后补"的入口，不是只在启动时设一次。
    void  SetHostWindowHandle(void* handle);
    void* GetHostWindowHandle() const { return m_HostWindowHandle; }

    /// 宿主表面的像素尺寸。无引擎窗口时 GetWindowSize() / GetFramebufferSize()
    /// 用它代替 1280x720 的兜底常量 —— 用兜底常量会让移动端交换链按错误分辨率
    /// 创建，旋转之后尤其明显。
    void SetHostWindowSize(int width, int height);

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
    // 触摸事件类型。移动端宿主推入（Android 的 AInputEvent / iOS 的 UITouch），
    // 桌面没有对应的输入源。
    enum class TouchAction : int
    {
        Down   = 0,
        Move   = 1,
        Up     = 2,
        /// 系统接管了这根手指（来电、下拉通知栏、多任务手势）。必须当作 Up
        /// 处理，否则引擎会一直以为手指还按着 —— 回到前台时表现为"角色自己在走"。
        Cancel = 3,
    };

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
    // pointer_id, action(TouchAction), x, y —— x/y 与鼠标光标同一坐标空间（像素）。
    typedef std::function<void(int,int,float,float)> OnTouchFunc;

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
    void RegisterOnTouchFunc(OnTouchFunc func)      { m_OnTouchFunc.push_back(func); }

    /// 宿主（移动端）推入触摸事件。
    ///
    /// 桌面由 GenericWindow 的回调驱动，用不到这个 —— 移动端根本没有
    /// GenericWindow（CreatePlatformApplication() 返回 nullptr），只能由宿主
    /// 直接喂进来。Player 壳的 ZPlayer_Touch() 就是走这里。
    void PushTouchEvent(int pointer_id, int action, float x, float y);

    bool IsMouseButtonDown(int button) const;

    /// 窗口是否处于"焦点模式"（光标锁定、输入直通）。
    ///
    /// 没有引擎自有的窗口时恒为 true —— 这一支服务的是移动端 / Web /
    /// 外部宿主驱动的配置（CreatePlatformApplication() 返回 nullptr）。那里
    /// 根本不存在"窗口失焦"这回事，返回 false 会让 InputSystem 每帧给
    /// GameCommand 打上 invalid 位（InputSystem::Tick），
    /// MotorComponent::TickPlayerMotor 因此整体 return：表现为"角色不动，
    /// 但日志里没有任何错误"。
    bool GetFocusMode() const { return m_Application == nullptr || m_IsFocusMode; }

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
    void OnTouch(int pointer_id, int action, float x, float y);

    std::unique_ptr<GenericApplication> m_Application;
    bool                                   m_IsFocusMode{false};
    SplashScreen*                          m_SplashScreen{nullptr};

    /// 宿主推入的绘制表面（移动端）。引擎不拥有它，也不负责释放。
    void* m_HostWindowHandle {nullptr};
    int   m_HostWidth {0};
    int   m_HostHeight {0};

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
    std::vector<OnTouchFunc>           m_OnTouchFunc;
};
