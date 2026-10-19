#pragma once

#include "../Generic/GenericWindow.h"

#include <array>
#include <string>

class GenericApplication;

// macOS 窗口实现（原生 Cocoa，不依赖 GLFW），参考 WindowsWindow / UE FMacWindow。
// 与 Win32 侧保持同一套对外契约：
//   * GetNativeHandle() 返回平台原生句柄（macOS = NSWindow*）
//   * 键码 / action / 修饰键用 Runtime/Function/Input/KeyCodes.h（GLFW 兼容值）
//   * 光标坐标原点在左上角（同 Win32 客户区 / GLFW）
class MacOSWindow : public GenericWindow
{
public:
    MacOSWindow() = default;
    ~MacOSWindow() override;

    // 创建窗口。parent_window 为 NSWindow*（子窗口 owner），可为空。
    bool Initialize(const char* title,
                    int         width,
                    int         height,
                    bool        decorated     = true,
                    bool        visible       = false,
                    bool        focus_on_show = true,
                    void*       parent_window = nullptr);

    // GenericWindow 接口
    void*              GetNativeHandle() const override;
    void               SetTitle(const char* title) override;
    void               Show() override;
    void               Hide() override;
    void               Destroy() override;
    bool               IsVisible() const override;
    std::array<int, 2> GetSize() const override;
    std::array<int, 2> GetPosition() const override;
    void               SetSize(int width, int height) override;
    void               SetPosition(int x, int y) override;
    std::array<int, 2> GetFramebufferSize() const override;
    float              GetDpiScale() const override;
    void               GetCursorPos(double& x, double& y) const override;
    bool               ShouldClose() const override;

    // 承载 CAMetalLayer 的 Cocoa 视图（NSView*）
    void* GetNativeView() const;

    void SetShouldClose(bool v);
    void SetApplication(GenericApplication* app) { m_Application = app; }
    GenericApplication* GetApplication() const { return m_Application; }

private:
    void*               m_NativeWindow {nullptr};  // NSWindow*（未持有，Destroy 时释放）
    GenericApplication* m_Application {nullptr};
    bool                m_ShouldClose {false};
};
