#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

// 抽象窗口接口，参考 UE FGenericWindow
class GenericWindow
{
public:
    virtual ~GenericWindow() = default;

    // 平台原生句柄（Windows=HWND, macOS=NSWindow*, Linux=Window）
    virtual void* GetNativeHandle() const = 0;

    // 窗口操作
    virtual void  SetTitle(const char* title) = 0;
    virtual void  Show() = 0;
    virtual void  Hide() = 0;
    virtual void  Destroy() = 0;
    virtual bool  IsVisible() const = 0;

    virtual std::array<int, 2> GetSize() const = 0;
    virtual std::array<int, 2> GetPosition() const = 0;
    virtual void              SetSize(int width, int height) = 0;
    virtual void              SetPosition(int x, int y) = 0;

    // framebuffer 像素尺寸（考虑 DPI 缩放）
    virtual std::array<int, 2> GetFramebufferSize() const = 0;

    // DPI scale factor
    virtual float GetDpiScale() const { return 1.0f; }

    // 光标位置（窗口客户区坐标，等价 glfwGetCursorPos）
    // 需要在消息循环中更新，或通过 GetCursorPos API
    virtual void GetCursorPos(double& x, double& y) const = 0;

    // 窗口是否请求关闭（等价 glfwWindowShouldClose）
    virtual bool ShouldClose() const = 0;

    // 回调注册（每窗口独立，替代 GLFW 的 glfwSetXxxCallback）
    // 主窗口回调通过 WindowSystem::registerOnXxxFunc 注册，内部转发到主窗口的 GenericWindow
    // 子窗口回调通过 GenericWindow::SetXxxCallback 设置
    std::vector<std::function<void(int, int, int, int)>> OnKey;
    std::vector<std::function<void(unsigned int)>>          OnChar;
    std::vector<std::function<void(int, unsigned int)>>     OnCharMods;
    std::vector<std::function<void(int, int, int)>>       OnMouseButton;
    std::vector<std::function<void(double, double)>>       OnCursorPos;
    std::vector<std::function<void(int)>>                 OnCursorEnter;
    std::vector<std::function<void(double, double)>>       OnScroll;
    std::vector<std::function<void(int, const char**)>>    OnDrop;
    std::vector<std::function<void(int, int)>>            OnWindowSize;
    std::vector<std::function<void()>>                    OnWindowClose;
    std::vector<std::function<void(int)>>                 OnWindowFocus;
    std::vector<std::function<void()>>                    OnWindowRefresh;
};
