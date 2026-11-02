#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

    // 抽象窗口接口，参考 UE FGenericWindow
class GenericWindow
{
public:
    // 光标锁定模式。值与 Unity 的 `CursorLockMode` 一致（None / Locked /
    // Confined），脚本侧的 `Cursor.lockState` 直接 static_cast 过来。
    //
    // 为什么是嵌套在 GenericWindow 里而不是全局：脚本侧也要一个叫
    // `CursorLockMode` 的枚举（ZENUM 生成 `declare enum`），两个同名的全局
    // 枚举会打架。窗口层这一个的拼写是 `GenericWindow::CursorLockMode`。
    enum class CursorLockMode : int
    {
        None     = 0,
        Locked   = 1,
        Confined = 2,
    };

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

    // ---- 光标状态（Unity 的 Cursor.visible / Cursor.lockState）----------
    //
    // 默认实现**只记录不生效**：后端没有真正去改 OS 光标时，读回来仍然是"脚
    // 本设的那个值"，而不是"OS 实际是什么"。这样脚本层的契约（设什么读回什么）
    // 在所有平台上都成立，而"是否真的生效"由各后端自己 override。
    //
    // 与 `Screen.SetFullScreen` 采取相反的处理（那里是干脆不导出）：光标状态
    // 是**上层策略**（"进游戏了，锁光标"），缺了它脚本根本没有表达这个意图的
    // 地方；而全屏是窗口后端的硬能力，导出了就是一句空承诺。
    virtual void SetCursorVisible(bool visible) { m_CursorVisible = visible; }
    virtual bool IsCursorVisible() const { return m_CursorVisible; }

    virtual void SetCursorLockMode(CursorLockMode mode) { m_CursorLockMode = mode; }
    virtual CursorLockMode GetCursorLockMode() const { return m_CursorLockMode; }

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

protected:
    // 见上面 SetCursorVisible / SetCursorLockMode 的说明：这两个成员是"脚本
    // 设过什么"的记录，后端 override 之后还会额外产生真实的 OS 效果。
    bool           m_CursorVisible {true};
    CursorLockMode m_CursorLockMode {CursorLockMode::None};
};
