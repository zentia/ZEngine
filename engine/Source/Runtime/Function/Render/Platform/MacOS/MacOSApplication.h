#pragma once

#include "../Generic/GenericApplication.h"
#include "MacOSWindow.h"

#include <filesystem>
#include <functional>
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

    // 双击 .zproject，而编辑器**已经在跑**的那条路。
    //
    // 启动时那条路是 Launcher 接的（ZEditorLauncher.mm 的 -application:openFile:），
    // 但 Launcher 只在没有 -p 时跑一次；一旦编辑器起来了，系统再发
    // kAEOpenDocuments 过来就没有 Launcher 可接了。所以平台层把它接住、缓起来，
    // 交给 Application 主循环里每帧必调的 PollEvents() 去消费 —— 不用新开一条
    // 每帧通道，也不会漏。
    //
    // 平台层不做"这是不是项目文件"的判断，那是 Editor 的事（它才知道 .zproject
    // 长什么样、当前开的是哪个项目）。
    using OpenDocumentHandler = std::function<void(const std::filesystem::path&)>;
    static void SetOpenDocumentHandler(OpenDocumentHandler handler);

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
