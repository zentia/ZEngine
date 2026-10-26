#pragma once

#include "../Generic/GenericWindow.h"
#include <string>

// X11 窗口实现，参考 UE FX11Window。
//
// 头文件刻意不 include <X11/Xlib.h>：Display / Window / XEvent 等类型只在 .cpp 里
// 出现，头里用不透明句柄（Display* -> void*，Window -> unsigned long）保存，这样本头
// 在任何后端都能被 include（Windows/Apple/Web 的构建会 glob 到本目录的 .cpp，但内容
// 整体被 Z_PLATFORM_LINUX && !__EMSCRIPTEN__ 守卫掉，非 Linux 上是空 TU）。
//
// 事件模型与 Windows/MacOS 不同：X11 是"拉"模型，事件经 XNextEvent/XPending 在
// LinuxApplication::PollEvents() 里统一抽取并分发到各窗口的回调（见那侧的 X event
// filter）。本窗口只保存状态 + 提供回调向量（继承自 GenericWindow）。
class LinuxWindow : public GenericWindow
{
public:
    LinuxWindow() = default;
    virtual ~LinuxWindow() override = default;

    // 创建 X11 窗口。decorated=false -> 无窗口装饰（弹窗）。
    // 返回 false = 无法创建（无 display / 窗口管理器拒绝）。
    bool Initialize(const char* title,
                    int         width,
                    int         height,
                    bool        decorated   = true,
                    bool        visible     = false,
                    bool        focus_on_show = true);

    // GenericWindow 接口
    void*              GetNativeHandle() const override { return reinterpret_cast<void*>(m_Window); }
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
    bool               ShouldClose() const override { return m_ShouldClose; }
    void               SetShouldClose(bool v) { m_ShouldClose = v; }

    // 供 PollEvents 取回 this（X11 事件按 Window 句柄路由）
    unsigned long GetXWindow() const { return m_Window; }
    void SetParentApplication(class LinuxApplication* app) { m_Application = app; }

private:
    // X11 句柄（不透明）。真实类型在 .cpp 里。
    void*          m_Display {nullptr};   // Display*
    unsigned long  m_Window {0};          // Window
    class LinuxApplication* m_Application {nullptr};

    int   m_Width {0};
    int   m_Height {0};
    bool  m_ShouldClose {false};
    bool  m_Mapped {false};   // XMapWindow 过
};
