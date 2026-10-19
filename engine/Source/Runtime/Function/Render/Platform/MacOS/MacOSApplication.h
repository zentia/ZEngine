#pragma once

#include "../Generic/GenericApplication.h"
#include "MacOSWindow.h"

#include <memory>
#include <vector>

// macOS 应用实现（原生 Cocoa 消息泵 + NSWindow），参考 WindowsApplication /
// UE FMacApplication。与 Win32 侧对称：每个平台用各自的原生窗口系统，不引入
// GLFW 之类的第三方窗口库。
// macOS 此前没有平台实现：WindowSystem::CreatePlatformApplication() 直接返回
// nullptr，导致编辑器拿不到窗口。
class MacOSApplication : public GenericApplication
{
public:
    MacOSApplication();
    ~MacOSApplication() override;

    // GenericApplication 接口
    bool Initialize(const char* title, int width, int height) override;
    void Shutdown() override;
    void PollEvents() override;
    bool ShouldClose() const override;
    void RequestClose() override;
    void SetTitle(const char* title) override;
    void ShowMainWindow() override;
    void SetCursorMode(bool capture) override;

    // 剪贴板
    void         SetClipboardText(const char* text) override;
    std::string  GetClipboardText() override;

    // 光标
    void*  CreateStandardCursor(int shape) override;
    void   SetCursor(void* window_handle, void* cursor) override;
    double GetTime() override;

    // 子窗口
    GenericWindow* CreateChildWindow(const char* title,
                                     int         width,
                                     int         height,
                                     int         pos_x,
                                     int         pos_y,
                                     bool        decorated = true) override;
    void DestroyChildWindow(GenericWindow* window) override;

    GenericWindow*     GetMainWindow() const override;
    std::array<int, 2> GetFramebufferSize() const override;

    // 显示器信息
    int         GetMonitorCount() const override;
    MonitorInfo GetPrimaryMonitorWorkArea() const override;
    MonitorInfo GetMonitorWorkArea(int monitor_index) const override;
    MonitorInfo GetMonitorWorkAreaForWindow(GenericWindow* window) const override;

private:
    std::unique_ptr<MacOSWindow>                m_MainWindow;
    std::vector<std::unique_ptr<MacOSWindow>>   m_OwnedChildren;
    bool                                        m_ShouldClose {false};
    bool                                        m_CursorCaptured {false};
    static bool                                 s_CocoaInitialized;
};
