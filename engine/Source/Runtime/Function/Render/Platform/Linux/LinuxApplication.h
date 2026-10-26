#pragma once

#include "../Generic/GenericApplication.h"
#include "LinuxWindow.h"
#include <memory>
#include <vector>

// X11 应用实现，参考 UE FX11Application。
// 拥有 Display、消息循环（XNextEvent 拉模型）和所有窗口。
//
// 与 Windows 的 PeekMessage 泵不同，X11 用 XPending/XNextEvent 阻塞抽取事件；
// PollEvents() 抽干当前队列后分发到各窗口（按 Window 句柄路由到 LinuxWindow::
// ProcessEvent），并顺带把主窗口的 ShouldClose 反映到 m_ShouldClose。
class LinuxApplication : public GenericApplication
{
public:
    LinuxApplication();
    ~LinuxApplication() override;

    // 供 LinuxWindow 取回共享的 Display*（XOpenDisplay 一次，全应用复用）。
    void* GetDisplay() const { return m_Display; }

    // GenericApplication::GetNativeDisplay —— 供 VulkanRHI 建 xlib surface。
    void* GetNativeDisplay() const override { return m_Display; }

    // GenericApplication 接口
    bool Initialize(const char* title, int width, int height) override;
    void Shutdown() override;
    void PollEvents() override;
    bool ShouldClose() const override { return m_ShouldClose; }
    void RequestClose() override { m_ShouldClose = true; }
    void SetTitle(const char* title) override;
    void ShowMainWindow() override;
    void SetCursorMode(bool capture) override;
    bool IsCursorCaptured() const { return m_CursorCaptured; }

    // 剪贴板（X11 selection：CLIPBOARD 选择 + UTF8_STRING 目标）。
    void          SetClipboardText(const char* text) override;
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

    // 显示器信息（XRandR；顺带覆盖 C7 的多显示器枚举子项）。
    int         GetMonitorCount() const override;
    MonitorInfo GetPrimaryMonitorWorkArea() const override;
    MonitorInfo GetMonitorWorkArea(int monitor_index) const override;
    MonitorInfo GetMonitorWorkAreaForWindow(GenericWindow* window) const override;

private:
    void*                            m_Display {nullptr};   // Display*
    std::unique_ptr<LinuxWindow>     m_MainWindow;
    std::vector<GenericWindow*>      m_AllWindows;          // 弱引用，PollEvents 分发
    std::vector<std::unique_ptr<GenericWindow>> m_OwnedChildren;
    bool                             m_ShouldClose {false};
    bool                             m_CursorCaptured {false};
};
