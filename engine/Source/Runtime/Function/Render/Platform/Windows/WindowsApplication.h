#pragma once

#include "../Generic/GenericApplication.h"
#include "WindowsWindow.h"
#include <memory>
#include <vector>

// Win32 应用实现，参考 UE FWindowsApplication
// 拥有消息循环和所有窗口
class WindowsApplication : public GenericApplication
{
public:
    WindowsApplication();
    ~WindowsApplication() override;

    // GenericApplication 接口
    bool Initialize(const char* title, int width, int height) override;
    void Shutdown() override;
    void PollEvents() override;
    bool ShouldClose() const override { return m_ShouldClose; }
    void RequestClose() override { m_ShouldClose = true; }
    void SetTitle(const char* title) override;
    void ShowMainWindow() override;
    void SetCursorMode(bool capture) override;

    // 剪贴板
    void           SetClipboardText(const char* text) override;
    std::string   GetClipboardText() override;

    // 光标
    void* CreateStandardCursor(int shape) override;
    void  SetCursor(void* window_handle, void* cursor) override;

    double GetTime() override;

    GenericWindow* CreateChildWindow(const char* title,
                                    int        width,
                                    int        height,
                                    int        pos_x,
                                    int        pos_y,
                                    bool       decorated = true) override;
    void             DestroyChildWindow(GenericWindow* window) override;

    GenericWindow*    GetMainWindow() const override;
    std::array<int, 2> GetFramebufferSize() const override;

    // 显示器信息
    int         GetMonitorCount() const override;
    MonitorInfo GetPrimaryMonitorWorkArea() const override;
    MonitorInfo GetMonitorWorkArea(int monitor_index) const override;
    MonitorInfo GetMonitorWorkAreaForWindow(GenericWindow* window) const override;

private:
    std::unique_ptr<WindowsWindow>        m_MainWindow;
    std::vector<GenericWindow*>          m_AllWindows;  // 弱引用，用于 PollEvents 分发
    std::vector<std::unique_ptr<GenericWindow>> m_OwnedChildren;  // 拥有的子窗口
    bool                                   m_ShouldClose {false};
    bool                                   m_CursorCaptured {false};
};
