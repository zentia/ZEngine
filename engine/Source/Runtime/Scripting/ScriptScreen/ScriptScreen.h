#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include "ScriptScreen.generated.h"

// 脚本侧的屏幕 / 窗口查询（Unity 的 `Screen` 静态类）。
//
// 为什么是 `GetWidth()` 而不是 `width`
// -------------------------------------
// 同 `Time.GetDeltaTime()` / `Application.GetDataPath()`：ZSCRIPTPROPERTY 的
// getter 不允许 static，静态属性做不出来，只能落成静态方法。
//
// 单位：屏幕坐标，不是像素
// -------------------------------------
// `GetWidth()`/`GetHeight()` 是**窗口客户区**尺寸（Screen Coordinates），
// `GetFramebufferWidth()/Height()` 才是**实际渲染像素**（DPI 缩放之后）。
// 在 200% 缩放的 Windows 机器上前者 1280、后者 2560。布局用前者，
// "我要一张多大的 render target"用后者。混用表现为高分屏上 UI 只有一半大。
//
// 没有 `SetFullScreen` / `GetFullScreen`
// ---------------------------------------
// 引擎的窗口后端（WindowsWindow / MacOSWindow / Linux）目前**没有**全屏切换
// —— GenericWindow 里连个虚函数都没有。与其返回一个永远 false 的假值，不如
// 先不导出：脚本写了 `Screen.SetFullScreen(true)` 却什么都没发生，比
// "编译期就告诉我没有这个 API" 难查得多。要补的时候从给 GenericWindow 加
// `SetFullScreen` 虚函数开始，三个后端各自实现。
//
// 没有 `Screen.resolutions`
// -------------------------
// 同理：枚举显示器模式需要 GLFW 的 glfwGetVideoModes 一类的后端支持。
// `GetMonitorCount()` / `GetPrimaryMonitorWidth()` 这些已经由
// GenericApplication 提供了，先给这些。
//
// header-only（同 ScriptTime.h / ScriptInput.h）—— 它只是一层转发，没有状态，
// 也没有对应的 .cpp。

ZCLASS()
class Screen : public Object
{
    ZGENERATED_BODY(Screen)

public:
    // ---- 窗口 / 屏幕尺寸 ----

    /// 窗口客户区宽（屏幕坐标，非像素）。Unity 的 `Screen.width`。
    ZFUNCTION()
    static int GetWidth()
    {
        if (auto* window_system = GET_SYSTEM(WindowSystem))
        {
            return window_system->GetWindowSize()[0];
        }
        return 0;
    }

    /// 窗口客户区高（屏幕坐标，非像素）。Unity 的 `Screen.height`。
    ZFUNCTION()
    static int GetHeight()
    {
        if (auto* window_system = GET_SYSTEM(WindowSystem))
        {
            return window_system->GetWindowSize()[1];
        }
        return 0;
    }

    /// 实际渲染缓冲宽（像素，已含 DPI 缩放）。建 render target / 截图时用。
    ZFUNCTION()
    static int GetFramebufferWidth()
    {
        if (auto* window_system = GET_SYSTEM(WindowSystem))
        {
            return window_system->GetFramebufferSize()[0];
        }
        return 0;
    }

    /// 实际渲染缓冲高（像素，已含 DPI 缩放）。
    ZFUNCTION()
    static int GetFramebufferHeight()
    {
        if (auto* window_system = GET_SYSTEM(WindowSystem))
        {
            return window_system->GetFramebufferSize()[1];
        }
        return 0;
    }

    /// DPI 缩放系数（1.0 = 100%）。Unity 的 `Screen.dpi` 是绝对 DPI，这里是
    /// 缩放比 —— 后者才是 UI 缩放真正要用的量，也是各后端唯一能可靠给出的。
    ZFUNCTION()
    static float GetDpi()
    {
        auto* window_system = GET_SYSTEM(WindowSystem);
        if (window_system == nullptr)
        {
            return 1.0f;
        }
        if (GenericWindow* window = window_system->GetMainWindow())
        {
            return window->GetDpiScale();
        }
        // 无引擎窗口（移动端 / 宿主表面）：缩放比由 framebuffer 与窗口尺寸的
        // 比值反推，除不动就当 1。
        const int window_width = window_system->GetWindowSize()[0];
        const int buffer_width = window_system->GetFramebufferSize()[0];
        if (window_width > 0 && buffer_width > 0)
        {
            return static_cast<float>(buffer_width) / static_cast<float>(window_width);
        }
        return 1.0f;
    }

    // ---- 显示器 ----

    /// 显示器数量。
    ZFUNCTION()
    static int GetMonitorCount()
    {
        auto* window_system = GET_SYSTEM(WindowSystem);
        if (window_system == nullptr)
        {
            return 0;
        }
        if (GenericApplication* app = window_system->GetApplication())
        {
            return app->GetMonitorCount();
        }
        return 0;
    }

    /// 主显示器工作区宽（不含任务栏 / Dock）。
    ZFUNCTION()
    static int GetPrimaryMonitorWidth()
    {
        return PrimaryMonitorWorkArea().width;
    }

    /// 主显示器工作区高（不含任务栏 / Dock）。
    ZFUNCTION()
    static int GetPrimaryMonitorHeight()
    {
        return PrimaryMonitorWorkArea().height;
    }

    /// 主显示器工作区左上角 X。多显示器下用它定位窗口。
    ZFUNCTION()
    static int GetPrimaryMonitorX()
    {
        return PrimaryMonitorWorkArea().x;
    }

    /// 主显示器工作区左上角 Y。
    ZFUNCTION()
    static int GetPrimaryMonitorY()
    {
        return PrimaryMonitorWorkArea().y;
    }

    // ---- 设置 ----

    /// 设置窗口尺寸（屏幕坐标）。Unity 的 `Screen.SetResolution(w, h, fullscreen)`
    /// 的前两参 —— 第三参（全屏）上面说了，暂不支持。
    ///
    /// 只改客户区，不动位置：改位置会把窗口甩到别处，而调用方通常只想"换个
    /// 分辨率"。没有引擎窗口时（移动端 / 宿主表面）是空操作 —— 那里的尺寸由
    /// 宿主决定，引擎改不动，硬要改只会在下一次 surfaceChanged 被覆盖回去。
    ZFUNCTION()
    static void SetResolution(int width, int height)
    {
        auto* window_system = GET_SYSTEM(WindowSystem);
        if (window_system == nullptr)
        {
            return;
        }
        if (GenericWindow* window = window_system->GetMainWindow())
        {
            // 0 / 负数会交给平台 API，Windows 上是创建一个 0 宽的 HWND
            // （随后所有基于尺寸的除法都是 0 除）。夹到 1，宁可小也不能 0。
            window->SetSize(width > 0 ? width : 1, height > 0 ? height : 1);
        }
    }

private:
    /// 主显示器工作区。拿不到时返回全 0 —— 与"存在但尺寸为 0"同值，但这在
    /// 实践中不会出现（没有显示器时 GetMonitorCount() 也是 0）。
    static GenericApplication::MonitorInfo PrimaryMonitorWorkArea()
    {
        auto* window_system = GET_SYSTEM(WindowSystem);
        if (window_system != nullptr)
        {
            if (GenericApplication* app = window_system->GetApplication())
            {
                return app->GetPrimaryMonitorWorkArea();
            }
        }
        return GenericApplication::MonitorInfo {0, 0, 0, 0};
    }
};
