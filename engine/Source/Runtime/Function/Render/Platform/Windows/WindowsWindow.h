#pragma once

#include "../Generic/GenericWindow.h"
#include <windows.h>
#include <string>

// Win32 窗口实现，参考 UE FWindowsWindow
class WindowsWindow : public GenericWindow
{
public:
    WindowsWindow() = default;
    virtual ~WindowsWindow() = default;

    // 初始化（创建 HWND），等价于 glfwCreateWindow
    // `main_window`=true 时注册所有回调用的窗口类
    bool Initialize(const char* title,
                  int         width,
                  int         height,
                  bool        decorated   = true,
                  bool        visible      = false,
                  bool        focus_on_show = true,
                  HWND        parent = nullptr);

    // GenericWindow 接口实现
    void*             GetNativeHandle() const override { return m_Hwnd; }
    void              SetTitle(const char* title) override;
    void              Show() override;
    void              Hide() override;
    void              Destroy() override;
    bool              IsVisible() const override;
    std::array<int, 2> GetSize() const override;
    std::array<int, 2> GetPosition() const override;
    void              SetSize(int width, int height) override;
    void              SetPosition(int x, int y) override;
    std::array<int, 2> GetFramebufferSize() const override;
    float             GetDpiScale() const override;

    // 供 WndProc 调用，分发消息到回调
    bool ProcessMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outResult);

    // 访问器
    HWND GetHwnd() const { return m_Hwnd; }
    void SetParentApplication(class WindowsApplication* app) { m_Application = app; }

    // 光标位置（客户区坐标）
    void GetCursorPos(double& x, double& y) const override;

    // 窗口是否请求关闭
    bool ShouldClose() const override { return m_ShouldClose; }
    void SetShouldClose(bool v) { m_ShouldClose = v; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND                m_Hwnd {nullptr};
    class WindowsApplication* m_Application {nullptr};
    std::wstring        m_WindowClassName;
    int                  m_Width {0};
    int                  m_Height {0};
    float                m_DpiScale {1.0f};
    bool                 m_ShouldClose {false};

    // 追踪鼠标按钮状态（等价于 glfwGetMouseButton）
    bool m_MouseButtons[3] {false, false, false};
};
