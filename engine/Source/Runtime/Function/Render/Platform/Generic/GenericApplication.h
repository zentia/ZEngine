#pragma once

#include <array>
#include <functional>
#include <set>
#include <string>
#include "GenericWindow.h"

// 抽象应用接口（消息循环 + 窗口管理），参考 UE FGenericApplication
class GenericApplication
{
public:
    virtual ~GenericApplication() = default;

    // 初始化主窗口
    virtual bool Initialize(const char* title, int width, int height) = 0;

    // 关闭、清理
    virtual void Shutdown() = 0;

    // 消息泵，等价于 glfwPollEvents()
    virtual void PollEvents() = 0;

    // 主窗口是否请求关闭
    virtual bool ShouldClose() const = 0;

    // 请求关闭主窗口
    virtual void RequestClose() = 0;

    // 设置主窗口标题
    virtual void SetTitle(const char* title) = 0;

    // 显示主窗口（splash 之后调用）
    virtual void ShowMainWindow() = 0;

    // 鼠标捕获模式（等价于 glfwSetInputMode(window, GLFW_CURSOR, ...)）
    virtual void SetCursorMode(bool capture) = 0;

    // ===== 子窗口（编辑器浮窗）=====
    // 创建子窗口，等价于 glfwCreateWindow（带 GLFW_NO_API hint）
    // 返回 GenericWindow*，调用方负责 DestroyChildWindow
    virtual GenericWindow* CreateChildWindow(const char*  title,
                                           int        width,
                                           int        height,
                                           int        pos_x,
                                           int        pos_y,
                                           bool       decorated = true) = 0;

    // 销毁子窗口
    virtual void DestroyChildWindow(GenericWindow* window) = 0;

    // ===== 访问器 =====
    virtual GenericWindow* GetMainWindow() const = 0;

    virtual std::array<int, 2> GetFramebufferSize() const = 0;

    // 剪贴板（系统级，非 per-window）
    virtual void           SetClipboardText(const char* text) = 0;
    virtual std::string   GetClipboardText() = 0;

    // 键盘状态查询（等价于 glfwGetKey）
    // 需要在 OnKey 回调中维护 m_KeyDown 集合
    virtual bool IsKeyDown(int key) const {
        return m_KeyDown.find(key) != m_KeyDown.end();
    }
    // 鼠标按钮状态查询（等价于 glfwGetMouseButton）
    virtual bool IsMouseButtonDown(int button) const {
        return m_MouseDown.find(button) != m_MouseDown.end();
    }

    // 光标（等价 glfwCreateStandardCursor / glfwSetCursor）
    virtual void* CreateStandardCursor(int shape) = 0;  // shape: 0=arrow, 1=hand, 2=hresize, 3=vresize, 4=nwse-resize
    virtual void  SetCursor(void* window_handle, void* cursor) = 0;
    virtual double GetTime() = 0;

    // ===== 显示器信息（等价 glfwGetMonitors / glfwGetMonitorWorkarea）=====
    struct MonitorInfo
    {
        int x, y;       // 显示器工作区左上角（不含任务栏）
        int width, height; // 工作区尺寸
    };
    virtual int         GetMonitorCount() const = 0;
    virtual MonitorInfo GetPrimaryMonitorWorkArea() const = 0;
    virtual MonitorInfo GetMonitorWorkArea(int monitor_index) const = 0;

    // 查找包含指定窗口中心的显示器
    virtual MonitorInfo GetMonitorWorkAreaForWindow(GenericWindow* window) const = 0;

    // ===== 键盘/鼠标状态维护（由 WindowSystem::OnKey/OnMouseButton 调用）=====
    void UpdateKeyState(int key, int action) {
        if (action == 1) m_KeyDown.insert(key);       // PRESS
        else m_KeyDown.erase(key);                        // RELEASE
    }
    void UpdateMouseState(int button, int action) {
        if (action == 1) m_MouseDown.insert(button);
        else m_MouseDown.erase(button);
    }

protected:
    std::set<int> m_KeyDown;   // 当前按下的键（GLFW 兼容键码）
    std::set<int> m_MouseDown; // 当前按下的鼠标按钮
};
